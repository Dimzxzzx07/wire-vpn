import { EventEmitter } from 'events';
import * as dgram from 'dgram';
import * as fs from 'fs';
import * as path from 'path';

const nativeModule = require('../../build/Release/wirevpn.node');

export interface VPNPeer {
  publicKey: string;
  endpoint: string;
  allowedIPs: string[];
}

export interface VPNConfig {
  privateKey?: string;
  publicKey?: string;
  address: string;
  port: number;
  peers: VPNPeer[];
  mtu?: number;
}

export class VPNConnection extends EventEmitter {
  private tun: any;
  private crypto: any;
  private stats: any;
  private running: boolean = false;
  private debugMode: boolean = false;
  private udpSocket: any;
  private config: VPNConfig;
  private peerMap: Map<string, VPNPeer>;
  private readInterval: NodeJS.Timeout | null = null;

  constructor(config: VPNConfig, debug: boolean = false) {
    super();
    this.config = config;
    this.debugMode = debug;
    this.tun = new nativeModule.TunDevice();
    this.crypto = new nativeModule.CryptoEngine();
    this.stats = nativeModule.StatsCollector;
    this.peerMap = new Map();
    this.udpSocket = null;
  }

  async init(): Promise<void> {
    let privateKey = this.config.privateKey;
    if (!privateKey) {
      const keypair = this.crypto.generateKeypair();
      privateKey = keypair.privateKey.toString('hex');
      this.config.privateKey = privateKey;
      this.config.publicKey = keypair.publicKey.toString('hex');
      console.log(`Generated new keypair - Public: ${this.config.publicKey}`);
    } else {
      this.crypto.setPrivateKey(Buffer.from(privateKey, 'hex'));
    }
    
    for (const peer of this.config.peers) {
      this.crypto.setPeerPublicKey(Buffer.from(peer.publicKey, 'hex'));
      this.peerMap.set(peer.publicKey, peer);
    }
  }

  async up(): Promise<void> {
    const tunFd = this.tun.open();
    if (tunFd < 0) {
      throw new Error('Failed to open TUN device');
    }
    
    this.tun.configure(this.config.address, this.config.mtu || 1500);
    
    this.udpSocket = dgram.createSocket('udp4');
    
    await new Promise<void>((resolve) => {
      this.udpSocket.bind(this.config.port, () => resolve());
    });
    
    this.running = true;
    
    this.udpSocket.on('message', (data: Buffer, rinfo: any) => {
      const decrypted = this.crypto.decrypt(data);
      if (decrypted && decrypted.length > 0) {
        const peerKey = this.getPeerKey(rinfo.address, rinfo.port);
        if (this.stats && this.stats.recordRx) {
          this.stats.recordRx(peerKey, decrypted.length, `${rinfo.address}:${rinfo.port}`);
        }
        this.tun.write(decrypted);
      }
    });
    
    this.readInterval = setInterval(() => {
      if (!this.running) return;
      
      const packet = this.tun.read();
      if (packet && packet.length > 0) {
        if (this.debugMode) {
          const hexdump = this.tun.getHexDump(packet);
          console.log(`[DEBUG] TUN packet (${packet.length} bytes):\n${hexdump}`);
        }
        
        const encrypted = this.crypto.encrypt(packet);
        if (encrypted && encrypted.length > 0) {
          for (const peer of this.peerMap.values()) {
            const [host, port] = peer.endpoint.split(':');
            this.udpSocket.send(encrypted, parseInt(port), host);
            if (this.stats && this.stats.recordTx) {
              this.stats.recordTx(peer.publicKey, encrypted.length, peer.endpoint);
            }
          }
        }
      }
    }, 10);
    
    console.log(`VPN UP - Interface: ${this.tun.name}, IP: ${this.config.address}, Port: ${this.config.port}`);
  }

  async down(): Promise<void> {
    this.running = false;
    
    if (this.readInterval) {
      clearInterval(this.readInterval);
      this.readInterval = null;
    }
    
    if (this.udpSocket) {
      this.udpSocket.close();
      this.udpSocket = null;
    }
    
    this.tun.close();
    
    console.log('VPN DOWN - Interface closed');
  }

  getStatus(): any {
    if (this.stats && this.stats.getStats) {
      return this.stats.getStats();
    }
    return {};
  }

  getPeers(): string[] {
    if (this.stats && this.stats.getAllPeers) {
      return this.stats.getAllPeers();
    }
    return [];
  }

  private getPeerKey(ip: string, port: number): string {
    for (const [key, peer] of this.peerMap.entries()) {
      if (peer.endpoint === `${ip}:${port}`) {
        return key;
      }
    }
    return `${ip}:${port}`;
  }
}
