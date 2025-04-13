{
  "targets": [
    {
      "target_name": "pcsc",      // Eklenti adı (NODE_API_MODULE ile aynı olmalı)
      "sources": [ "pcsc.cc" ], // C++ kaynak dosyanızın adı

      // Node-API başlık dosyalarının yolunu ekle
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include\")"
      ],

      // Node-API bağımlılığını belirt
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],

      // İşletim sistemine özel ayarlar
      "conditions": [
        // Windows Ayarları
        ['OS=="win"', {
          "libraries": [
            "-lWinscard" // Winscard.lib kütüphanesine bağlan
          ],
           # Windows için MSVC derleyici ayarları
          'msvs_settings': {
            'VCCLCompilerTool': {
               # /EHsc bayrağına karşılık gelir (C++ istisnalarını etkinleştirir)
              'ExceptionHandling': 1
            }
          }
        }],

        // Linux Ayarları
        ['OS=="linux"', {
          "libraries": [
            "-lpcsclite" // PCSC-lite kütüphanesine bağlan
          ],
           # Linux için GCC/Clang derleyici ayarları (genellikle varsayılanlar yeterlidir)
           # C++ istisnaları genellikle varsayılan olarak etkindir.
        }],

        // macOS Ayarları
        ['OS=="mac" or OS=="darwin"', { // macOS için 'darwin' daha yaygındır
          "libraries": [
             "-lpcsclite" // PCSC-lite kütüphanesine bağlan (Homebrew vb. ile kurulmuşsa)
             // Alternatif olarak, Framework kullanılabilir:
             // '-framework PCSC'
          ],
           # macOS için Xcode/Clang ayarları
          'xcode_settings': {
             # Clang'de C++ istisnalarını etkinleştir
            'GCC_ENABLE_CPP_EXCEPTIONS': 'YES',
             # C++ standart kütüphanesini belirt (genellikle libc++)
            'CLANG_CXX_LIBRARY': 'libc++',
             # Minimum macOS sürümünü belirt (isteğe bağlı ama iyi pratik)
            'MACOSX_DEPLOYMENT_TARGET': '10.9', // Veya desteklemek istediğiniz minimum sürüm
          },
          # gyp'nin macOS'ta varsayılan olarak eklediği -fno-exceptions bayrağını kaldır
          'cflags!': [ '-fno-exceptions' ],
          'cflags_cc!': [ '-fno-exceptions' ],
        }]
      ], // conditions sonu
      

      // N-API için C++ istisnalarını devre dışı BıRAKMAYIN (eğer C++ kodu istisna kullanıyorsa)
      // Eğer kodunuz istisna kullanmıyorsa 'NAPI_DISABLE_CPP_EXCEPTIONS' tanımlamak daha güvenli olabilir.
      // Ancak PC/SC kütüphaneleri veya std kütüphanesi istisna fırlatabilir.
      // Bu yüzden ExceptionHandling'i platformlara göre yönetmek daha doğru.
      // "defines": [ 'NAPI_DISABLE_CPP_EXCEPTIONS' ], // Şimdilik yorum satırı yapalım veya kaldıralım

      // Genel C/C++ derleyici bayrakları (gerekiyorsa)
      # 'cflags': [],
      # 'cflags_cc': [],

    }
  ]
} 