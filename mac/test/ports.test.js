import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { deviceName, identify, rank } from '../src/transport/portDiscovery.js';

describe('matching the two nodes of one device', () => {
  it('sees through the tty./cu. split', () => {
    // serialport reports /dev/tty.*, the rest of the daemon uses /dev/cu.*.
    // Matching on the full path silently names nothing at all.
    assert.equal(deviceName('/dev/tty.usbmodem2101'), deviceName('/dev/cu.usbmodem2101'));
  });

  it('keeps distinct devices distinct', () => {
    assert.notEqual(deviceName('/dev/cu.usbmodem2101'), deviceName('/dev/cu.usbserial-0001'));
  });
});

describe('port ranking by filename', () => {
  it('prefers a UART bridge over a bare usbmodem', () => {
    const ports = rank(['cu.usbmodem2101', 'cu.usbserial-0001']);
    assert.equal(ports[0]?.path, '/dev/cu.usbserial-0001');
  });

  it('drops the ports that are never the board', () => {
    const ports = rank(['cu.Bluetooth-Incoming-Port', 'cu.debug-console', 'tty.usbserial-1']);
    assert.deepEqual(ports, []);
  });
});

describe('naming a port from its USB ids', () => {
  it('names the board behind Espressif’s USB-Serial/JTAG', () => {
    // What the C3 and S3 expose. The descriptor is in ROM and reads
    // "USB JTAG/serial debug unit" everywhere else, which is the whole reason
    // this mapping exists.
    const id = identify('303A', '1001');
    assert.match(id?.hint ?? '', /Chicken Micro board/);
  });

  it('outranks a UART bridge, so the C3 wins when both are plugged in', () => {
    const native = identify('303a', '1001');
    const bridge = identify('10c4', 'ea60');
    assert.ok(native && bridge);
    assert.ok(native.score > bridge.score);
  });

  it('falls back to the vendor when the product id is unknown', () => {
    assert.match(identify('303a', 'ffff')?.hint ?? '', /Espressif/);
  });

  it('is case-insensitive, since the two sources disagree on case', () => {
    assert.deepEqual(identify('10C4', 'EA60'), identify('10c4', 'ea60'));
  });

  it('says nothing about ids it does not know', () => {
    assert.equal(identify('1234', '5678'), null);
    assert.equal(identify(undefined, undefined), null);
  });
});
