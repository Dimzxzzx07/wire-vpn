#include <napi.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <string>
#include <cstring>
#include <vector>
#include <atomic>
#include <map>
#include <chrono>
#include <sodium.h>

class TunDevice : public Napi::ObjectWrap<TunDevice> {
private:
  int tun_fd;
  std::string iface_name;

public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "TunDevice", {
      InstanceMethod<&TunDevice::Open>("open"),
      InstanceMethod<&TunDevice::Read>("read"),
      InstanceMethod<&TunDevice::Write>("write"),
      InstanceMethod<&TunDevice::Configure>("configure"),
      InstanceMethod<&TunDevice::Close>("close"),
      InstanceMethod<&TunDevice::GetHexDump>("getHexDump"),
      InstanceAccessor<&TunDevice::GetName, nullptr>("name")
    });

    Napi::FunctionReference* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);
    exports.Set("TunDevice", func);
    return exports;
  }

  TunDevice(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TunDevice>(info) {
    tun_fd = -1;
  }

  Napi::Value Open(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    
    tun_fd = open("/dev/net/tun", O_RDWR);
    if (tun_fd < 0) {
      Napi::Error::New(env, "Failed to open TUN device").ThrowAsJavaScriptException();
      return env.Null();
    }
    
    if (ioctl(tun_fd, TUNSETIFF, &ifr) < 0) {
      close(tun_fd);
      Napi::Error::New(env, "Failed to configure TUN device").ThrowAsJavaScriptException();
      return env.Null();
    }
    
    iface_name = ifr.ifr_name;
    int flags = fcntl(tun_fd, F_GETFL, 0);
    fcntl(tun_fd, F_SETFL, flags | O_NONBLOCK);
    
    return Napi::Number::New(env, tun_fd);
  }
  
  Napi::Value Read(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (tun_fd < 0) return Napi::Buffer<char>::New(env, 0);
    
    char buffer[65536];
    ssize_t n = read(tun_fd, buffer, sizeof(buffer));
    if (n > 0) return Napi::Buffer<char>::Copy(env, buffer, n);
    return Napi::Buffer<char>::New(env, 0);
  }
  
  Napi::Value Write(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (tun_fd < 0 || info.Length() < 1) return Napi::Number::New(env, -1);
    
    Napi::Buffer<char> buffer = info[0].As<Napi::Buffer<char>>();
    ssize_t n = write(tun_fd, buffer.Data(), buffer.Length());
    return Napi::Number::New(env, n);
  }
  
  Napi::Value Configure(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 2) return env.Null();
    
    std::string ip = info[0].As<Napi::String>();
    int mtu = info[1].As<Napi::Number>().Int32Value();
    
    std::string cmd_ip = "ip addr add " + ip + " dev " + iface_name + " 2>/dev/null";
    system(cmd_ip.c_str());
    std::string cmd_up = "ip link set " + iface_name + " up 2>/dev/null";
    system(cmd_up.c_str());
    std::string cmd_mtu = "ip link set mtu " + std::to_string(mtu) + " dev " + iface_name + " 2>/dev/null";
    system(cmd_mtu.c_str());
    
    return env.Null();
  }
  
  Napi::Value GetHexDump(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1) return Napi::String::New(env, "");
    
    Napi::Buffer<char> buffer = info[0].As<Napi::Buffer<char>>();
    char* data = buffer.Data();
    size_t len = buffer.Length();
    
    std::string result;
    for (size_t i = 0; i < len && i < 256; i++) {
      char hex[4];
      snprintf(hex, sizeof(hex), "%02x ", (unsigned char)data[i]);
      result += hex;
      if ((i + 1) % 16 == 0) result += "\n";
    }
    return Napi::String::New(env, result);
  }
  
  void Close(const Napi::CallbackInfo& info) {
    if (tun_fd >= 0) {
      close(tun_fd);
      tun_fd = -1;
    }
  }
  
  Napi::Value GetName(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), iface_name);
  }
};

class CryptoEngine : public Napi::ObjectWrap<CryptoEngine> {
private:
  unsigned char private_key[crypto_box_SECRETKEYBYTES];
  unsigned char public_key[crypto_box_PUBLICKEYBYTES];
  unsigned char peer_public_key[crypto_box_PUBLICKEYBYTES];
  unsigned char shared_key[crypto_box_BEFORENMBYTES];
  bool shared_key_initialized;

public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "CryptoEngine", {
      InstanceMethod<&CryptoEngine::GenerateKeypair>("generateKeypair"),
      InstanceMethod<&CryptoEngine::SetPrivateKey>("setPrivateKey"),
      InstanceMethod<&CryptoEngine::SetPeerPublicKey>("setPeerPublicKey"),
      InstanceMethod<&CryptoEngine::Encrypt>("encrypt"),
      InstanceMethod<&CryptoEngine::Decrypt>("decrypt"),
      InstanceMethod<&CryptoEngine::GetPublicKey>("getPublicKey")
    });

    Napi::FunctionReference* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);
    exports.Set("CryptoEngine", func);
    return exports;
  }

  CryptoEngine(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CryptoEngine>(info) {
    if (sodium_init() < 0) {
      Napi::Error::New(info.Env(), "Failed to initialize libsodium").ThrowAsJavaScriptException();
    }
    shared_key_initialized = false;
    memset(shared_key, 0, crypto_box_BEFORENMBYTES);
  }

  Napi::Value GenerateKeypair(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    crypto_box_keypair(public_key, private_key);
    
    Napi::Object result = Napi::Object::New(env);
    result.Set("publicKey", Napi::Buffer<unsigned char>::Copy(env, public_key, crypto_box_PUBLICKEYBYTES));
    result.Set("privateKey", Napi::Buffer<unsigned char>::Copy(env, private_key, crypto_box_SECRETKEYBYTES));
    return result;
  }

  void SetPrivateKey(const Napi::CallbackInfo& info) {
    if (info.Length() < 1) return;
    Napi::Buffer<unsigned char> key = info[0].As<Napi::Buffer<unsigned char>>();
    if (key.Length() == crypto_box_SECRETKEYBYTES) {
      memcpy(private_key, key.Data(), crypto_box_SECRETKEYBYTES);
      crypto_scalarmult_base(public_key, private_key);
    }
  }

  void SetPeerPublicKey(const Napi::CallbackInfo& info) {
    if (info.Length() < 1) return;
    Napi::Buffer<unsigned char> key = info[0].As<Napi::Buffer<unsigned char>>();
    if (key.Length() != crypto_box_PUBLICKEYBYTES) return;
    
    memcpy(peer_public_key, key.Data(), crypto_box_PUBLICKEYBYTES);
    crypto_box_beforenm(shared_key, peer_public_key, private_key);
    shared_key_initialized = true;
  }

  Napi::Value Encrypt(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!shared_key_initialized || info.Length() < 1) {
      return Napi::Buffer<char>::New(env, 0);
    }
    
    Napi::Buffer<char> plaintext = info[0].As<Napi::Buffer<char>>();
    unsigned char nonce[crypto_box_NONCEBYTES];
    randombytes_buf(nonce, sizeof(nonce));
    
    size_t ciphertext_len = plaintext.Length() + crypto_box_MACBYTES;
    std::vector<unsigned char> ciphertext(ciphertext_len);
    
    crypto_box_afternm(ciphertext.data(), 
                       reinterpret_cast<unsigned char*>(plaintext.Data()), 
                       plaintext.Length(), 
                       nonce, 
                       shared_key);
    
    size_t result_len = sizeof(nonce) + ciphertext_len;
    std::vector<char> result(result_len);
    memcpy(result.data(), nonce, sizeof(nonce));
    memcpy(result.data() + sizeof(nonce), ciphertext.data(), ciphertext_len);
    
    return Napi::Buffer<char>::Copy(env, result.data(), result_len);
  }

  Napi::Value Decrypt(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!shared_key_initialized || info.Length() < 1) {
      return Napi::Buffer<char>::New(env, 0);
    }
    
    Napi::Buffer<char> ciphertext_with_nonce = info[0].As<Napi::Buffer<char>>();
    if (ciphertext_with_nonce.Length() < crypto_box_NONCEBYTES + crypto_box_MACBYTES) {
      return Napi::Buffer<char>::New(env, 0);
    }
    
    unsigned char nonce[crypto_box_NONCEBYTES];
    memcpy(nonce, ciphertext_with_nonce.Data(), crypto_box_NONCEBYTES);
    
    size_t ciphertext_len = ciphertext_with_nonce.Length() - crypto_box_NONCEBYTES;
    size_t plaintext_len = ciphertext_len - crypto_box_MACBYTES;
    std::vector<unsigned char> plaintext(plaintext_len);
    
    if (crypto_box_open_afternm(plaintext.data(),
                                reinterpret_cast<unsigned char*>(ciphertext_with_nonce.Data() + crypto_box_NONCEBYTES),
                                ciphertext_len,
                                nonce,
                                shared_key) != 0) {
      return Napi::Buffer<char>::New(env, 0);
    }
    
    return Napi::Buffer<char>::Copy(env, reinterpret_cast<char*>(plaintext.data()), plaintext_len);
  }

  Napi::Value GetPublicKey(const Napi::CallbackInfo& info) {
    return Napi::Buffer<unsigned char>::Copy(info.Env(), public_key, crypto_box_PUBLICKEYBYTES);
  }
};

struct PeerStats {
  std::atomic<uint64_t> tx_bytes{0};
  std::atomic<uint64_t> rx_bytes{0};
  std::atomic<uint64_t> tx_packets{0};
  std::atomic<uint64_t> rx_packets{0};
  std::chrono::steady_clock::time_point last_seen;
  std::string endpoint;
};

static std::map<std::string, PeerStats> global_stats;

class StatsCollector : public Napi::ObjectWrap<StatsCollector> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "StatsCollector", {
      StaticMethod<&StatsCollector::RecordTx>("recordTx"),
      StaticMethod<&StatsCollector::RecordRx>("recordRx"),
      StaticMethod<&StatsCollector::GetStats>("getStats"),
      StaticMethod<&StatsCollector::GetAllPeers>("getAllPeers"),
      StaticMethod<&StatsCollector::ResetStats>("resetStats")
    });

    exports.Set("StatsCollector", func);
    return exports;
  }

  StatsCollector(const Napi::CallbackInfo& info) : Napi::ObjectWrap<StatsCollector>(info) {}

  static Napi::Value RecordTx(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 2) return env.Undefined();
    
    std::string peerId = info[0].As<Napi::String>();
    uint64_t bytes = info[1].As<Napi::Number>().Uint32Value();
    
    auto& stats = global_stats[peerId];
    stats.tx_bytes += bytes;
    stats.tx_packets++;
    stats.last_seen = std::chrono::steady_clock::now();
    
    if (info.Length() >= 3) {
      stats.endpoint = info[2].As<Napi::String>();
    }
    return env.Undefined();
  }
  
  static Napi::Value RecordRx(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 2) return env.Undefined();
    
    std::string peerId = info[0].As<Napi::String>();
    uint64_t bytes = info[1].As<Napi::Number>().Uint32Value();
    
    auto& stats = global_stats[peerId];
    stats.rx_bytes += bytes;
    stats.rx_packets++;
    stats.last_seen = std::chrono::steady_clock::now();
    
    if (info.Length() >= 3) {
      stats.endpoint = info[2].As<Napi::String>();
    }
    return env.Undefined();
  }
  
  static Napi::Value GetStats(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object result = Napi::Object::New(env);
    
    for (auto& [peerId, stats] : global_stats) {
      Napi::Object peerStats = Napi::Object::New(env);
      peerStats.Set("txBytes", Napi::Number::New(env, (double)stats.tx_bytes.load()));
      peerStats.Set("rxBytes", Napi::Number::New(env, (double)stats.rx_bytes.load()));
      peerStats.Set("txPackets", Napi::Number::New(env, (double)stats.tx_packets.load()));
      peerStats.Set("rxPackets", Napi::Number::New(env, (double)stats.rx_packets.load()));
      peerStats.Set("endpoint", Napi::String::New(env, stats.endpoint));
      
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stats.last_seen).count();
      peerStats.Set("lastSeen", Napi::Number::New(env, (double)elapsed));
      result.Set(peerId, peerStats);
    }
    return result;
  }
  
  static Napi::Value GetAllPeers(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Array result = Napi::Array::New(env);
    uint32_t idx = 0;
    for (auto& [peerId, stats] : global_stats) {
      result.Set(idx++, Napi::String::New(env, peerId));
    }
    return result;
  }
  
  static Napi::Value ResetStats(const Napi::CallbackInfo& info) {
    global_stats.clear();
    return info.Env().Undefined();
  }
};

static Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  TunDevice::Init(env, exports);
  CryptoEngine::Init(env, exports);
  StatsCollector::Init(env, exports);
  return exports;
}

NODE_API_MODULE(vpn_native, InitAll)
