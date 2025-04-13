{
  "targets": [
    {
      "target_name": "pcsc",
      "sources": ["pcsc.cc"],
      "include_dirs": [
       "/Users/piton/Desktop/nodejs-nfc-reader/node_modules/node-addon-api/include",
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "conditions": [
        ["OS=='win'", {
          "libraries": ["-lwinscard"],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 0
            }
          }
        }],
        ['OS=="linux"', {
            "libraries": ["-lpcsclite"],
            'cflags': ['-fno-exceptions'],
            'cflags_cc': ['-fno-exceptions']
        }],
        ['OS=="mac" or OS=="darwin"', {
          'include_dirs': [
           "/Users/piton/Desktop/nodejs-nfc-reader/node_modules/node-addon-api/include",
            '/opt/homebrew/opt/pcsc-lite/include',
            '/opt/homebrew/include',
            '/usr/local/include'
          ],
          'library_dirs': [
            '/opt/homebrew/opt/pcsc-lite/lib',
            '/opt/homebrew/lib',
            '/usr/local/lib'
          ],
          "libraries": [
             "-lpcsclite"
          ],
          'xcode_settings': {
            'GCC_ENABLE_CPP_EXCEPTIONS': 'NO',
            'CLANG_CXX_LIBRARY': 'libc++',
            'MACOSX_DEPLOYMENT_TARGET': '10.13',
            'GCC_TREAT_WARNINGS_AS_ERRORS': 'NO',
            'WARNING_CFLAGS!': ['-Werror']
          },
          'cflags': ['-fno-exceptions'],
          'cflags_cc': ['-fno-exceptions']
        }]
      ]
    }
  ]
}