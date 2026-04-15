#!/usr/bin/env node

import { program } from 'commander';
import * as fs from 'fs';
import * as path from 'path';
import * as os from 'os';

const CONFIG_DIR = os.homedir() + '/.vpn';
const CONFIG_FILE = CONFIG_DIR + '/config.json';
const KEY_FILE = CONFIG_DIR + '/keys.json';

function ensureConfigDir() {
  if (!fs.existsSync(CONFIG_DIR)) {
    fs.mkdirSync(CONFIG_DIR, { recursive: true });
  }
}

function loadConfig(): any | null {
  try {
    if (fs.existsSync(CONFIG_FILE)) {
      return JSON.parse(fs.readFileSync(CONFIG_FILE, 'utf-8'));
    }
  } catch (e) {}
  return null;
}

let activeConnection: any = null;

program
  .name('vpn')
  .description('Native VPN CLI')
  .version('1.0.0');

program
  .command('init')
  .alias('i')
  .description('Generate new private and public keys')
  .action(async () => {
    ensureConfigDir();
    
    const nativeModule = require('../build/Release/wirevpn.node');
    const crypto = new nativeModule.CryptoEngine();
    const keypair = crypto.generateKeypair();
    
    const keys = {
      privateKey: keypair.privateKey.toString('hex'),
      publicKey: keypair.publicKey.toString('hex'),
      createdAt: new Date().toISOString()
    };
    
    fs.writeFileSync(KEY_FILE, JSON.stringify(keys, null, 2));
    
    console.log('Keys generated successfully!');
    console.log(`Public Key: ${keys.publicKey}`);
    console.log(`Private Key saved to: ${KEY_FILE}`);
  });

program
  .command('up <config>')
  .alias('u')
  .description('Start VPN connection')
  .option('-d, --debug', 'Enable debug mode')
  .action(async (configPath: string, options: any) => {
    let config;
    
    if (fs.existsSync(configPath)) {
      config = JSON.parse(fs.readFileSync(configPath, 'utf-8'));
    } else {
      const saved = loadConfig();
      if (saved) {
        config = saved;
      } else {
        console.error(`Config file not found: ${configPath}`);
        process.exit(1);
      }
    }
    
    if (fs.existsSync(KEY_FILE)) {
      const keys = JSON.parse(fs.readFileSync(KEY_FILE, 'utf-8'));
      config.privateKey = keys.privateKey;
      config.publicKey = keys.publicKey;
    }
    
    const { VPNConnection } = require('./lib/vpn');
    activeConnection = new VPNConnection(config, options.debug || false);
    
    try {
      await activeConnection.init();
      await activeConnection.up();
      
      console.log('\nVPN is running. Press Ctrl+C to stop.\n');
      
      const statusInterval = setInterval(() => {
        if (activeConnection) {
          const stats = activeConnection.getStatus();
          const peerList = activeConnection.getPeers();
          
          process.stdout.write(`\rActive: ${peerList.length} peers | `);
          for (const [key, stat] of Object.entries(stats)) {
            const s: any = stat;
            process.stdout.write(`TX: ${(s.txBytes / 1024).toFixed(1)}KB RX: ${(s.rxBytes / 1024).toFixed(1)}KB `);
          }
        }
      }, 1000);
      
      process.on('SIGINT', async () => {
        clearInterval(statusInterval);
        if (activeConnection) {
          await activeConnection.down();
        }
        console.log('\nVPN stopped.');
        process.exit(0);
      });
      
    } catch (err: any) {
      console.error(`Failed to start VPN: ${err.message}`);
      process.exit(1);
    }
  });

program
  .command('down')
  .alias('d')
  .description('Stop VPN connection')
  .action(async () => {
    if (activeConnection) {
      await activeConnection.down();
      activeConnection = null;
      console.log('VPN connection closed');
    } else {
      console.log('No active VPN connection found');
    }
  });

program
  .command('status')
  .alias('s')
  .description('Display statistics')
  .action(() => {
    if (!activeConnection) {
      console.log('VPN is not running. Use "vpn up" first.');
      return;
    }
    
    const stats = activeConnection.getStatus();
    
    if (Object.keys(stats).length === 0) {
      console.log('No active peers');
      return;
    }
    
    console.log('Peer Statistics:');
    for (const [peer, stat] of Object.entries(stats)) {
      const s: any = stat;
      console.log(`  ${peer.substring(0, 16)}...`);
      console.log(`    TX: ${(s.txBytes / 1024).toFixed(2)} KB (${s.txPackets} packets)`);
      console.log(`    RX: ${(s.rxBytes / 1024).toFixed(2)} KB (${s.rxPackets} packets)`);
      console.log(`    Endpoint: ${s.endpoint}`);
      console.log(`    Last seen: ${s.lastSeen}s ago\n`);
    }
  });

program
  .command('list')
  .alias('l')
  .description('List connected peers')
  .action(() => {
    if (!activeConnection) {
      console.log('VPN is not running. Use "vpn up" first.');
      return;
    }
    
    const peers = activeConnection.getPeers();
    
    if (peers.length === 0) {
      console.log('No peers connected');
    } else {
      console.log(`Connected peers (${peers.length}):`);
      peers.forEach((peer: string, idx: number) => {
        console.log(`  ${idx + 1}. ${peer.substring(0, 16)}...`);
      });
    }
  });

program.parse();
