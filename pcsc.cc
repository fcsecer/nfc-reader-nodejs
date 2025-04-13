#include <napi.h>
#include <napi-inl.h> // AsyncWorker vs. için

#include <sstream>
#include <vector>
#include <string>
#include <iostream>
#include <thread>
#include <memory>
#include <atomic>
#include <mutex>
#include <iomanip> // std::hex, std::setw, std::setfill
#include <cstring> // memset, strlen için
#include <limits>  // numeric_limits

// === Platforma Özel Dahil Etmeler ve Tip Tanımları ===
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winscard.h>
    using SCardLong = LONG;
    using SCardByte = BYTE;
    using SCardDword = DWORD;
    // SCARDHANDLE ve SCARDCONTEXT winscard.h'de tanımlı
#else // Linux veya macOS
    #include <PCSC/pcsclite.h> // Genellikle Homebrew veya sistem yolu
    #include <PCSC/reader.h>   // Genellikle Homebrew veya sistem yolu
    // Alternatif olarak: #include <pcsclite.h>, #include <reader.h>
    #include <stdint.h>        // uint32_t vs. için
    #include <stdlib.h>        // free için (SCardFreeMemory yerine)
    #include <errno.h>         // Hata kodları için

    using SCardLong = long;            // PCSC-lite genellikle long döner
    using SCardByte = unsigned char;   // Standart byte tanımı
    using SCardDword = uint32_t;       // Standart 32-bit unsigned
    // SCARDHANDLE ve SCARDCONTEXT pcsclite.h'de tanımlı

    // SCARD_AUTOALLOCATE Windows'a özel, PCSC-lite'da yok.
    // SCardFreeMemory yerine free kullanacağız (manuel ayırma yaparsak).
    // Veya PCSC-lite'ın kendi SCardFreeMemory'sini kullanacağız (eğer varsa ve uyumluysa).
    // Winscard API'leri genellikle SCard...A (ANSI) ve SCard...W (Wide) versiyonlarına sahiptir.
    // PCSC-lite sadece tek bir versiyona sahiptir (genellikle UTF-8 veya yerel kodlama beklenir).
    // Bu kodda ANSI/char* kullandığımız için PCSC-lite ile uyumlu olmalı.
#endif

namespace PcscAddon {

    std::atomic<bool> g_running{false}; // Listener durumu
    SCARDCONTEXT g_context = 0;         // Global PC/SC context
    std::thread g_pollThread;           // Listener thread'i

    std::mutex g_listenerMutex;         // Listener verilerini koruma

    // Aktif dinleyici bilgileri
    struct ListenerInfo {
        std::string readerName;
        Napi::ThreadSafeFunction uidCallback;
        Napi::ThreadSafeFunction errorCallback;
    };
    std::unique_ptr<ListenerInfo> g_activeListener = nullptr;

    // === Hata İşleme Yardımcıları ===

    std::string SCardErrorToString(SCardLong rv) {
        #ifdef _WIN32
        // Windows'a özel detaylı hata mesajı alma
        LPSTR errorMsg = nullptr;
        DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        HMODULE hWinscard = GetModuleHandleA("winscard.dll");
        if (hWinscard) flags |= FORMAT_MESSAGE_FROM_HMODULE;
        DWORD len = FormatMessageA(flags, hWinscard, rv, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&errorMsg, 0, NULL);
        if (len > 0 && errorMsg != nullptr) {
            std::string result(errorMsg);
            LocalFree(errorMsg);
            while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
                result.pop_back();
            }
            // Hata kodunu da ekle
            std::stringstream ss;
            ss << result << " (0x" << std::hex << std::uppercase << rv << ")";
            return ss.str();
        }
        #else
        // PCSC-lite için (pcsc_stringify_error kullanılabilir ama kütüphaneye bağlı)
        // Şimdilik sadece hex kodu döndürelim
        // const char* pcscError = pcsc_stringify_error(rv); // Eğer PCSC kütüphanesi linklenmişse
        // if (pcscError) return std::string(pcscError) + " (0x" + ... + ")";
        #endif

        // Genel veya Windows'ta mesaj bulunamayan durum
        std::stringstream ss;
        ss << "PC/SC Error Code: 0x" << std::hex << std::uppercase << rv;
        return ss.str();
    }

    void ThrowNapiError(const Napi::Env& env, const std::string& message, SCardLong rv = 0) {
        std::string fullMessage = message;
        if (rv != 0) {
            fullMessage += " (" + SCardErrorToString(rv) + ")";
        }
        Napi::Error::New(env, fullMessage).ThrowAsJavaScriptException();
    }

    // === Context Yönetimi ===

    bool EnsureContext(const Napi::Env* env = nullptr) {
         // Çok basit kontrol: Zaten varsa geçerli kabul et.
         // Daha sağlam bir çözüm: SCardIsValidContext kullanmak veya hata durumunda yeniden denemek.
        if (g_context != 0) {
            return true;
        }

        SCardLong rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &g_context);
        if (rv != SCARD_S_SUCCESS) {
            g_context = 0;
            std::cerr << "ERROR: Failed to establish PC/SC context! " << SCardErrorToString(rv) << std::endl;
            if (env) {
                ThrowNapiError(*env, "Failed to establish PC/SC context", rv);
            }
            return false;
        }
        std::cout << "INFO: PC/SC context established successfully." << std::endl;
        return true;
    }

    void CleanupContext(void* /*arg*/) {
        std::cout << "INFO: Cleaning up PC/SC context..." << std::endl;
        // Çalışan listener'ı durdur
        if (g_running.load()) {
            g_running = false;
            if (g_context != 0) {
                 // SCardCancel bloke edici SCardGetStatusChange'i iptal eder.
                 // Bu, context serbest bırakılmadan çağrılmalı.
                 SCardCancel(g_context);
            }
            if (g_pollThread.joinable()) {
                try { g_pollThread.join(); } catch(...) { /* Ignore errors on cleanup */ }
            }
        }
         // Listener bilgisini temizle
        {
            std::lock_guard<std::mutex> lock(g_listenerMutex);
            if (g_activeListener) {
                g_activeListener.reset(); // unique_ptr temizler
            }
        }
        // Context'i serbest bırak
        if (g_context != 0) {
            SCardReleaseContext(g_context);
            g_context = 0;
            std::cout << "INFO: PC/SC context released successfully." << std::endl;
        }
    }

    // === Platforma Bağımlı PCI İşaretçileri ===
    // SCardTransmit'te SCARD_IO_REQUEST yerine bunları kullanmak daha taşınabilir
    const SCARD_IO_REQUEST* GetPci(SCardDword protocol) {
        switch (protocol) {
            case SCARD_PROTOCOL_T0:
                return SCARD_PCI_T0;
            case SCARD_PROTOCOL_T1:
                return SCARD_PCI_T1;
            // case SCARD_PROTOCOL_RAW: // Gerekirse eklenebilir
            //     return SCARD_PCI_RAW;
            default:
                // Desteklenmeyen veya bilinmeyen protokol için null dönmek güvenli olabilir
                // veya T1'i varsayalım (daha modern)
                std::cerr << "WARN: Unknown or unsupported protocol " << protocol << ", using T1 PCI." << std::endl;
                return SCARD_PCI_T1;
        }
    }


    // === APDU Transmit Worker (Asenkron İşlem) ===

    class TransmitWorker : public Napi::AsyncWorker {
    public:
        TransmitWorker(Napi::Env env,
                       Napi::Promise::Deferred deferred,
                       const std::string& readerName,
                       const std::vector<SCardByte>& apduToSend)
            : Napi::AsyncWorker(env),
              deferred(deferred),
              readerName(readerName),
              apduToSend(apduToSend),
              responseApdu(),
              lastRv(SCARD_S_SUCCESS) {}

        ~TransmitWorker() override {} // Sanal yıkıcı

    protected:
        // Arka plan thread'inde çalışır
        void Execute() override {
            if (!EnsureContext()) {
                lastRv = SCARD_E_INVALID_HANDLE; // Veya uygun PCSC-lite kodu
                SetError("PC/SC context not established or invalid.");
                return;
            }

            SCARDHANDLE hCard = 0;
            SCardDword dwActiveProtocol = 0;

            // Karta bağlan (SCARD_SHARE_SHARED en yaygın)
            lastRv = SCardConnect(g_context, readerName.c_str(), SCARD_SHARE_SHARED,
                                  SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                                  &hCard, &dwActiveProtocol);

            if (lastRv != SCARD_S_SUCCESS) {
                SetError("Failed to connect to card in reader: " + readerName);
                return;
            }

            // RAII Guard ile bağlantının kesilmesini garanti et
            struct CardConnectionGuard {
                SCARDHANDLE& handleRef;
                CardConnectionGuard(SCARDHANDLE& h) : handleRef(h) {}
                ~CardConnectionGuard() {
                    if (handleRef != 0) {
                        SCardDisconnect(handleRef, SCARD_LEAVE_CARD);
                        handleRef = 0;
                    }
                }
            } guard(hCard);

            // Platforma uygun PCI yapısını al
            const SCARD_IO_REQUEST* pci = GetPci(dwActiveProtocol);
            if (!pci) {
                 // Bu durum aslında GetPci içinde handle ediliyor ama yine de kontrol
                 lastRv = SCARD_E_PROTO_MISMATCH; // Veya uygun hata
                 SetError("Failed to get valid PCI structure for the active protocol.");
                 return;
            }

            // Yanıt buffer'ını hazırla (256 veri + 2 SW = 258, 260 makul bir boyut)
            responseApdu.resize(260); // std::vector<SCardByte>
            SCardDword dwRecvLength = static_cast<SCardDword>(responseApdu.size());

            // APDU'yu gönder
            lastRv = SCardTransmit(hCard, pci, apduToSend.data(), (SCardDword)apduToSend.size(),
                                   nullptr, // Yanıt için ek IO isteği yok
                                   responseApdu.data(), &dwRecvLength);

            if (lastRv != SCARD_S_SUCCESS) {
                SetError("APDU transmit/receive failed");
                // Guard bağlantıyı kesecek
                return;
            }

            // Alınan verinin gerçek boyutuna küçült
            responseApdu.resize(dwRecvLength);
            // Guard bağlantıyı kesecek
        }

        // Ana thread'de çalışır (başarılıysa)
        void OnOK() override {
            Napi::Env env = Env();
            // Yanıtı Napi::Buffer olarak kopyala
            Napi::Buffer<SCardByte> resultBuffer = Napi::Buffer<SCardByte>::Copy(env, responseApdu.data(), responseApdu.size());
            deferred.Resolve(resultBuffer);
        }

        // Ana thread'de çalışır (hatalıysa)
        void OnError(const Napi::Error& e) override {
            Napi::Env env = Env();
            std::string errorMessage = e.Message();
            // PC/SC hata kodunu ekle (eğer set edilmişse)
            if (lastRv != SCARD_S_SUCCESS) {
                errorMessage += " (" + SCardErrorToString(lastRv) + ")";
            }
            deferred.Reject(Napi::Error::New(env, errorMessage).Value());
        }

    private:
        Napi::Promise::Deferred deferred;
        std::string readerName;
        std::vector<SCardByte> apduToSend;
        std::vector<SCardByte> responseApdu; // Alınan yanıt
        SCardLong lastRv; // Son PC/SC hata kodu
    };


    // === Kart Dinleme İş Parçacığı ===

    void PollForCard() {
        std::unique_ptr<ListenerInfo> listener;
        // Dinleyici bilgilerini güvenli kopyala
        {
            std::lock_guard<std::mutex> lock(g_listenerMutex);
            if (!g_activeListener || !g_activeListener->uidCallback || !g_activeListener->errorCallback) {
                 std::cerr << "ERROR: PollForCard started without a valid listener." << std::endl;
                 return;
            }
            // Kopyasını oluştur
            listener = std::make_unique<ListenerInfo>();
            listener->readerName = g_activeListener->readerName;
            listener->uidCallback = g_activeListener->uidCallback;
            listener->errorCallback = g_activeListener->errorCallback;
        }

        std::cout << "INFO: Listening for cards on reader: '" << listener->readerName << "'" << std::endl;

        // Platforma uygun SCARD_READERSTATE yapısı
        #ifdef _WIN32
            SCARD_READERSTATEA readerState; // ANSI version for char*
        #else
            SCARD_READERSTATE readerState;  // PCSC-lite standard version
        #endif
        memset(&readerState, 0, sizeof(readerState));
        readerState.szReader = listener->readerName.c_str();
        readerState.dwCurrentState = SCARD_STATE_UNAWARE; // Başlangıç durumu

        while (g_running.load()) {
            // Platforma uygun SCardGetStatusChange çağrısı
            #ifdef _WIN32
                SCardLong rv = SCardGetStatusChangeA(g_context, 1000, &readerState, 1); // 1 saniye timeout
            #else
                // PCSC-lite timeout ms değil, özel değerler veya sonsuz olabilir.
                // INFINITE genellikle tanımsız olabilir, uzun bir timeout kullanalım.
                // Veya SCardCancel'e güvenelim. 1000ms = 1 saniye.
                SCardLong rv = SCardGetStatusChange(g_context, 1000, &readerState, 1);
            #endif

            if (!g_running.load()) break; // Cancel sonrası kontrol

            if (rv == SCARD_E_CANCELLED) {
                std::cout << "INFO: SCardGetStatusChange cancelled (listener stopping)." << std::endl;
                break;
            } else if (rv == SCARD_E_TIMEOUT) {
                continue; // Timeout normal, döngüye devam
            } else if (rv == SCARD_E_UNKNOWN_READER || rv == SCARD_E_READER_UNAVAILABLE
                    #ifndef _WIN32 // PCSC-lite'da bu hata farklı olabilir veya olmayabilir
                    || rv == SCARD_E_COMM_DATA_LOST // Windows'a özgü olabilir
                    #endif
                    || rv == SCARD_E_NO_SERVICE // PCSC-lite daemon durmuş olabilir
            ) {
                 std::cerr << "ERROR: Reader '" << listener->readerName << "' unavailable or service stopped. " << SCardErrorToString(rv) << std::endl;
                 auto errorStr = new std::string("Error: Reader '" + listener->readerName + "' unavailable or PC/SC service stopped. " + SCardErrorToString(rv));
                 listener->errorCallback.BlockingCall(errorStr, [](Napi::Env env, Napi::Function jsCallback, std::string* msg) {
                     jsCallback.Call({Napi::String::New(env, *msg)});
                     delete msg;
                 });
                 g_running = false; // Hata sonrası dinleyiciyi durdur
                 break;
            } else if (rv == SCARD_E_INVALID_HANDLE) {
                 std::cerr << "ERROR: PC/SC context became invalid." << std::endl;
                 auto errorStr = new std::string("Critical Error: PC/SC context became invalid. Restart might be required.");
                 listener->errorCallback.BlockingCall(errorStr, [](Napi::Env env, Napi::Function jsCallback, std::string* msg) {
                     jsCallback.Call({Napi::String::New(env, *msg)});
                     delete msg;
                 });
                 g_running = false; // Hata sonrası dinleyiciyi durdur
                 break;
            } else if (rv != SCARD_S_SUCCESS) {
                // Diğer beklenmedik hatalar
                std::cerr << "ERROR: SCardGetStatusChange failed! " << SCardErrorToString(rv) << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Kısa bekleme
                continue;
            }

            // Durum değişikliğini işle (dwEventState alanı her iki platformda da var)
            if (readerState.dwEventState & SCARD_STATE_CHANGED) {
                 // Yeni durumu bir sonraki kontrol için sakla (dwCurrentState alanı da ortak)
                readerState.dwCurrentState = readerState.dwEventState;

                // Kart takılı ve sessiz değil mi?
                if (readerState.dwEventState & SCARD_STATE_PRESENT && !(readerState.dwEventState & SCARD_STATE_MUTE)) {
                    std::cout << "INFO: Card detected in reader: " << listener->readerName << std::endl;
                    SCARDHANDLE hCard = 0;
                    SCardDword dwActiveProtocol = 0;

                    // Karta bağlan (SCardConnect her iki platformda da var)
                    rv = SCardConnect(g_context, listener->readerName.c_str(), SCARD_SHARE_SHARED,
                                      SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &hCard, &dwActiveProtocol);

                    if (rv == SCARD_S_SUCCESS) {
                        // UID APDU (platformdan bağımsız)
                        SCardByte cmd_get_uid[] = { 0xFF, 0xCA, 0x00, 0x00, 0x00 };
                        // Yanıt buffer'ı
                        std::vector<SCardByte> recvBufferVec(260);
                        SCardDword recvLength = static_cast<SCardDword>(recvBufferVec.size());

                        // Platforma uygun PCI yapısını al
                        const SCARD_IO_REQUEST* pci = GetPci(dwActiveProtocol);
                        if(!pci) {
                            rv = SCARD_E_PROTO_MISMATCH; // Hata kodu ata
                        } else {
                            // APDU gönder
                            rv = SCardTransmit(hCard, pci, cmd_get_uid, sizeof(cmd_get_uid),
                                               nullptr, recvBufferVec.data(), &recvLength);
                        }


                        if (rv == SCARD_S_SUCCESS && recvLength >= 2) {
                            // UID'yi formatla
                            std::stringstream uidStream;
                            uidStream << std::hex << std::uppercase << std::setfill('0');
                            for (SCardDword i = 0; i < recvLength - 2; i++) {
                                uidStream << std::setw(2) << static_cast<int>(recvBufferVec[i]);
                            }
                            auto uidStr = new std::string(uidStream.str());

                            // JS'e gönder (ThreadSafeFunction)
                            Napi::Error napiStatus = listener->uidCallback.BlockingCall(uidStr, [](Napi::Env env, Napi::Function jsCallback, std::string* data) {
                                jsCallback.Call({Napi::String::New(env, *data)});
                                delete data; // Heap'teki string'i sil
                            });

                            if (napiStatus != napi_ok) {
                                 std::cerr << "ERROR: Failed to call UID callback: " << napiStatus << std::endl;
                            }
                            // Tekrar okumayı önlemek için bekleme ve state sıfırlama
                            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                            readerState.dwCurrentState = SCARD_STATE_UNAWARE;

                        } else if (rv != SCARD_S_SUCCESS) {
                             // Transmit hatası
                            std::cerr << "ERROR: Failed to get UID (SCardTransmit): " << SCardErrorToString(rv) << std::endl;
                            auto errorStr = new std::string("Error: Failed to read UID from card. " + SCardErrorToString(rv));
                            listener->errorCallback.BlockingCall(errorStr, [](Napi::Env env, Napi::Function jsCallback, std::string* msg) {
                                jsCallback.Call({Napi::String::New(env, *msg)});
                                delete msg;
                            });
                        } else { // recvLength < 2 durumu
                             std::cerr << "WARN: SCardTransmit succeeded but received less than 2 bytes." << std::endl;
                        }

                        SCardDisconnect(hCard, SCARD_LEAVE_CARD);
                    } else {
                        // Connect hatası
                        std::cerr << "ERROR: Failed to connect to card (SCardConnect): " << SCardErrorToString(rv) << std::endl;
                        auto errorStr = new std::string("Error: Failed to connect to card. " + SCardErrorToString(rv));
                        listener->errorCallback.BlockingCall(errorStr, [](Napi::Env env, Napi::Function jsCallback, std::string* msg) {
                            jsCallback.Call({Napi::String::New(env, *msg)});
                            delete msg;
                        });
                        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Hata sonrası bekleme
                    }
                } else if (readerState.dwEventState & SCARD_STATE_EMPTY) {
                    std::cout << "INFO: Card removed from reader: " << listener->readerName << std::endl;
                }
            } // if (state changed)
        } // while (g_running)

        std::cout << "INFO: Listener stopped for reader: '" << listener->readerName << "'" << std::endl;

        // TSFL'leri serbest bırak (önemli!)
        if (listener) {
            if (listener->uidCallback) listener->uidCallback.Release();
            if (listener->errorCallback) listener->errorCallback.Release();
        }
    }


    // === N-API Fonksiyonları ===

    Napi::Value GetAllReaders(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!EnsureContext(&env)) return env.Null();

        char* readers = nullptr;
        SCardDword readersLen = 0; // Boyutu saklamak için

        #ifdef _WIN32
            // Windows: SCARD_AUTOALLOCATE kullan
            readersLen = SCARD_AUTOALLOCATE;
            SCardLong rv = SCardListReadersA(g_context, NULL, (LPSTR)&readers, &readersLen);
        #else
            // PCSC-lite: İki adımlı işlem
            // 1. Adım: Boyutu al
            SCardLong rv = SCardListReaders(g_context, NULL, NULL, &readersLen);
            if (rv == SCARD_S_SUCCESS && readersLen > 1) { // Boyut geçerliyse
                 // 2. Adım: Belleği ayır ve okuyucuları al
                 readers = new (std::nothrow) char[readersLen]; // Bellek ayırma hatasını kontrol et
                 if (readers == nullptr) {
                     rv = SCARD_E_NO_MEMORY; // Veya uygun hata kodu
                 } else {
                     rv = SCardListReaders(g_context, NULL, readers, &readersLen);
                 }
            } else if (rv == SCARD_E_NO_READERS_AVAILABLE) {
                 // Okuyucu yoksa sorun değil, rv'yi başarıya ayarla ki aşağıda boş dizi dönsün
                 rv = SCARD_S_SUCCESS;
                 readersLen = 0; // Emin olmak için
            }
            // Diğer SCardListReaders hataları (ilk çağrıda) rv içinde kalır
        #endif

        if (rv != SCARD_S_SUCCESS) {
            #ifndef _WIN32
            // PCSC-lite için ayrılan belleği temizle (eğer varsa)
            delete[] readers;
            #endif
            if (rv == SCARD_E_NO_READERS_AVAILABLE) {
                return Napi::Array::New(env); // Okuyucu yoksa boş dizi
            }
            ThrowNapiError(env, "Failed to list readers", rv);
            return env.Null();
        }

        Napi::Array result = Napi::Array::New(env);
        if (readers != nullptr && readersLen > 1) { // Multi-string buffer boş değilse
            char* currentReader = readers;
            uint32_t index = 0;
            while (*currentReader != '\0') { // Son çift null'a kadar git
                result.Set(index++, Napi::String::New(env, currentReader));
                currentReader += strlen(currentReader) + 1;
            }
        }

        // Ayrılan belleği serbest bırak
        #ifdef _WIN32
            if (readers != nullptr) SCardFreeMemory(g_context, readers);
        #else
            delete[] readers; // new[] ile ayrıldığı için delete[] ile sil
        #endif

        return result;
    }

    Napi::Value StartListening(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() < 3 || !info[0].IsString() || !info[1].IsFunction() || !info[2].IsFunction()) {
            Napi::TypeError::New(env, "Parameters expected: readerName (string), onUid (function), onError (function)").ThrowAsJavaScriptException();
            return env.Null();
        }
        if (!EnsureContext(&env)) return env.Null();
        if (g_running.load()) {
            Napi::Error::New(env, "Listener is already active. Call stopListening first.").ThrowAsJavaScriptException();
            return env.Null();
        }
        if (g_pollThread.joinable()) { // Önceki thread bitmemişse bekle (nadiren olmalı)
            try { g_pollThread.join(); } catch (...) {}
        }

        std::string readerName = info[0].As<Napi::String>().Utf8Value();
        Napi::Function uidCallback = info[1].As<Napi::Function>();
        Napi::Function errorCallback = info[2].As<Napi::Function>();

        Napi::ThreadSafeFunction tsfnUid = Napi::ThreadSafeFunction::New(env, uidCallback, "PCSC_UID_Callback", 0, 1);
        Napi::ThreadSafeFunction tsfnError = Napi::ThreadSafeFunction::New(env, errorCallback, "PCSC_Error_Callback", 0, 1);
        if (!tsfnUid || !tsfnError) {
            if (tsfnUid) tsfnUid.Release(); // Başarısız olursa temizle
            if (tsfnError) tsfnError.Release();
            Napi::Error::New(env, "Failed to create ThreadSafeFunctions.").ThrowAsJavaScriptException();
            return env.Null();
        }

        {
            std::lock_guard<std::mutex> lock(g_listenerMutex);
            g_activeListener = std::make_unique<ListenerInfo>();
            g_activeListener->readerName = readerName;
            g_activeListener->uidCallback = tsfnUid;
            g_activeListener->errorCallback = tsfnError;
        }

        g_running = true;
        try {
            g_pollThread = std::thread(PollForCard);
        } catch (const std::system_error& e) {
            g_running = false;
            tsfnUid.Abort(); // Abort, Release'i de yapar
            tsfnError.Abort();
            { std::lock_guard<std::mutex> lock(g_listenerMutex); g_activeListener.reset(); }
            ThrowNapiError(env, "Failed to start listener thread: " + std::string(e.what()));
            return env.Null();
        }
        return env.Null();
    }

    Napi::Value StopListening(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!g_running.load()) return env.Null(); // Zaten çalışmıyor

        g_running = false; // Önce flag'i ayarla

        if (g_context != 0) {
            SCardLong rv = SCardCancel(g_context); // SCardGetStatusChange'i uyandır/iptal et
            if (rv != SCARD_S_SUCCESS && rv != SCARD_E_INVALID_HANDLE) {
                 // PCSC-lite'da SCARD_W_CANCELLED_BY_USER olmayabilir, bu yüzden kontrol etme
                std::cerr << "WARN: SCardCancel failed: " << SCardErrorToString(rv) << std::endl;
            }
        }

        if (g_pollThread.joinable()) {
            try {
                g_pollThread.join(); // Thread'in bitmesini bekle
            } catch (const std::system_error& e) {
                std::cerr << "ERROR: Failed joining listener thread: " << e.what() << std::endl;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_listenerMutex);
            g_activeListener.reset(); // Listener bilgisini temizle
        }
        std::cout << "INFO: Listener stopped successfully." << std::endl;
        return env.Null();
    }

    Napi::Value TransmitAPDU(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsBuffer()) {
             Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
             deferred.Reject(Napi::TypeError::New(env, "Parameters expected: readerName (string), apdu (Buffer)").Value());
             return deferred.Promise();
        }
        if (!EnsureContext(&env)) {
             Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
             deferred.Reject(Napi::Error::New(env, "PC/SC context not established or invalid.").Value());
             return deferred.Promise();
        }

        std::string readerName = info[0].As<Napi::String>().Utf8Value();
        Napi::Buffer<SCardByte> apduNapiBuffer = info[1].As<Napi::Buffer<SCardByte>>();

        // Buffer verisini std::vector'e kopyala
        std::vector<SCardByte> apduToSend(apduNapiBuffer.Data(), apduNapiBuffer.Data() + apduNapiBuffer.Length());

        // AsyncWorker'ı başlat ve Promise'i döndür
        Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
        TransmitWorker* worker = new TransmitWorker(env, deferred, readerName, apduToSend);
        worker->Queue();
        return deferred.Promise();
    }

    // === Modül Başlatma ===

    Napi::Object Init(Napi::Env env, Napi::Object exports) {
        EnsureContext(); // Başlangıçta context kurmayı dene (hata göz ardı edilir)

        napi_status status = napi_add_env_cleanup_hook(env, CleanupContext, nullptr);
        if (status != napi_ok) {
             std::cerr << "WARN: Failed to add environment cleanup hook for PC/SC context." << std::endl;
        }

        exports.Set("getAllReaders", Napi::Function::New(env, GetAllReaders, "getAllReaders"));
        exports.Set("startListening", Napi::Function::New(env, StartListening, "startListening"));
        exports.Set("stopListening", Napi::Function::New(env, StopListening, "stopListening"));
        exports.Set("transmit", Napi::Function::New(env, TransmitAPDU, "transmit"));

        return exports;
    }

} // namespace PcscAddon

// Modülü Node.js'e kaydet
NODE_API_MODULE(pcsc, PcscAddon::Init)