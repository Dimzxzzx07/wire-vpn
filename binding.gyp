{
  "targets": [
    {
      "target_name": "wirevpn",
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "sources": [
        "src/addon/vpn.cc"
      ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "libraries": [
        "-lsodium"
      ],
      "conditions": [
        ["OS=='linux'", {
          "libraries": ["-lsodium"]
        }],
        ["OS=='mac'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LIBRARY": "libc++",
            "MACOSX_DEPLOYMENT_TARGET": "10.15"
          },
          "libraries": ["-lsodium"]
        }]
      ]
    }
  ]
}
