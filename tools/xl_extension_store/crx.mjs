// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import fs from 'node:fs';
import crypto from 'node:crypto';
import zlib from 'node:zlib';

export const GOOGLE_PUBLISHER_SHA256 =
    '61f7f2a6bfcf74cd0bc1fe2497cc9b04254c658f79f2145392867ea8366367cf';
export const MAX_CRX_SIZE = 256 * 1024 * 1024;
const CONTEXT = Buffer.from('CRX3 SignedData\0');

export function sha256(data) {
  return crypto.createHash('sha256').update(data).digest('hex');
}

function varint(value) {
  const bytes = [];
  do {
    bytes.push((value & 127) | (value > 127 ? 128 : 0));
    value = Math.floor(value / 128);
  } while (value);
  return Buffer.from(bytes);
}

function readVarint(data, position) {
  let value = 0;
  let shift = 0;
  for (let count = 0; count < 8; ++count) {
    if (position >= data.length) throw new Error('Truncated protobuf varint');
    const byte = data[position++];
    value += (byte & 127) * 2 ** shift;
    if (!(byte & 128)) {
      if (!Number.isSafeInteger(value)) throw new Error('Oversized protobuf value');
      return [value, position];
    }
    shift += 7;
  }
  throw new Error('Oversized protobuf varint');
}

export function protobufField(number, data) {
  return Buffer.concat([varint(number * 8 + 2), varint(data.length), data]);
}

function protobufFields(data) {
  const fields = [];
  let position = 0;
  while (position < data.length) {
    let tag;
    [tag, position] = readVarint(data, position);
    const number = Math.floor(tag / 8);
    const wire = tag & 7;
    if (!number) throw new Error('Invalid protobuf field');
    let value;
    if (wire === 2) {
      let length;
      [length, position] = readVarint(data, position);
      if (length > data.length - position) throw new Error('Truncated protobuf field');
      value = data.subarray(position, position + length);
      position += length;
    } else if (wire === 0) {
      [value, position] = readVarint(data, position);
    } else if (wire === 1 || wire === 5) {
      const length = wire === 1 ? 8 : 4;
      if (length > data.length - position) throw new Error('Truncated protobuf field');
      value = data.subarray(position, position + length);
      position += length;
    } else {
      throw new Error('Unsupported protobuf wire type');
    }
    fields.push({number, wire, value});
  }
  return fields;
}

function uniqueBytes(fields, number, label) {
  const values = fields.filter(field => field.number === number);
  if (values.length !== 1 || values[0].wire !== 2) {
    throw new Error(`Missing or duplicate ${label}`);
  }
  return values[0].value;
}

export function extensionId(idBytes) {
  return [...idBytes].map(byte =>
    String.fromCharCode(97 + (byte >> 4), 97 + (byte & 15))).join('');
}

export function signedPayload(signedHeader, archive) {
  const length = Buffer.alloc(4);
  length.writeUInt32LE(signedHeader.length);
  return Buffer.concat([CONTEXT, length, signedHeader, archive]);
}

export function parseCrx(data) {
  if (data.length > MAX_CRX_SIZE || data.length < 12 ||
      data.toString('ascii', 0, 4) !== 'Cr24' || data.readUInt32LE(4) !== 3) {
    throw new Error('Expected a CRX3 file of at most 256 MiB');
  }
  const headerSize = data.readUInt32LE(8);
  if (headerSize > 1024 * 1024 || headerSize > data.length - 12) {
    throw new Error('Invalid CRX3 header length');
  }
  const header = data.subarray(12, 12 + headerSize);
  const archive = data.subarray(12 + headerSize);
  const fields = protobufFields(header);
  const signedHeader = uniqueBytes(fields, 10000, 'signed_header_data');
  const idBytes = uniqueBytes(protobufFields(signedHeader), 1, 'crx_id');
  if (idBytes.length !== 16) throw new Error('CRX ID must contain 16 bytes');
  const proofs = fields.filter(field => field.number === 2 || field.number === 3)
    .map(field => {
      if (field.wire !== 2) throw new Error('Invalid signature proof');
      const proof = protobufFields(field.value);
      return {
        algorithm: field.number === 2 ? 'rsa' : 'ecdsa',
        publicKey: uniqueBytes(proof, 1, 'proof public_key'),
        signature: uniqueBytes(proof, 2, 'proof signature'),
      };
    });
  return {header, archive, signedHeader, idBytes, id: extensionId(idBytes), proofs};
}

export function verifyCrx(data, xlPublisherHash, requirePublisher = true) {
  const parsed = parseCrx(data);
  const payload = signedPayload(parsed.signedHeader, parsed.archive);
  let developer = false;
  let publisher = null;
  if (!parsed.proofs.length) throw new Error('CRX3 has no signature proofs');
  for (const proof of parsed.proofs) {
    const key = crypto.createPublicKey({
      key: proof.publicKey, type: 'spki', format: 'der',
    });
    if ((proof.algorithm === 'rsa' && key.asymmetricKeyType !== 'rsa') ||
        (proof.algorithm === 'ecdsa' && key.asymmetricKeyType !== 'ec')) {
      throw new Error('Proof algorithm and public key do not match');
    }
    // Chromium's CRX3 verifier uses RSA PKCS#1 v1.5 SHA256 or ECDSA SHA256.
    if (!crypto.verify('sha256', payload, key, proof.signature)) {
      throw new Error('CRX3 signature verification failed');
    }
    const hash = sha256(proof.publicKey);
    if (hash.slice(0, 32) === parsed.idBytes.toString('hex')) developer = true;
    if (hash === GOOGLE_PUBLISHER_SHA256) publisher = 'Google';
    else if (hash === xlPublisherHash && publisher !== 'Google') publisher = 'XL';
  }
  if (!developer) throw new Error('Missing valid developer proof matching CRX ID');
  if (requirePublisher && !publisher) {
    throw new Error('A verified Google or XL publisher proof is required');
  }
  return {...parsed, publisher, sha256: sha256(data)};
}

export function addPublisherProof(data, privateKeyPem, expectedPublisherHash) {
  const parsed = verifyCrx(data, expectedPublisherHash, false);
  const privateKey = crypto.createPrivateKey(privateKeyPem);
  const publicKey = crypto.createPublicKey(privateKey).export({
    type: 'spki', format: 'der',
  });
  if (sha256(publicKey) !== expectedPublisherHash) {
    throw new Error('Publisher private key does not match the pinned public key');
  }
  if (privateKey.asymmetricKeyType !== 'ec') {
    throw new Error('XL publisher key must be ECDSA P-256');
  }
  if (parsed.proofs.some(proof => sha256(proof.publicKey) === expectedPublisherHash)) {
    return data;
  }
  const signature = crypto.sign('sha256',
    signedPayload(parsed.signedHeader, parsed.archive), privateKey);
  const proof = Buffer.concat([
    protobufField(1, publicKey), protobufField(2, signature),
  ]);
  // Preserve every original header byte, including developer/Google proofs and
  // verified_contents. The signed header and ZIP remain byte-for-byte identical.
  const header = Buffer.concat([parsed.header, protobufField(3, proof)]);
  const prefix = Buffer.alloc(12);
  prefix.write('Cr24');
  prefix.writeUInt32LE(3, 4);
  prefix.writeUInt32LE(header.length, 8);
  const result = Buffer.concat([prefix, header, parsed.archive]);
  verifyCrx(result, expectedPublisherHash);
  return result;
}

function crc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) {
      crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function readZip(archive) {
  let end = -1;
  for (let pos = archive.length - 22;
       pos >= Math.max(0, archive.length - 65557); --pos) {
    if (archive.readUInt32LE(pos) === 0x06054b50 &&
        pos + 22 + archive.readUInt16LE(pos + 20) === archive.length) {
      end = pos;
      break;
    }
  }
  if (end < 0) throw new Error('Invalid ZIP end record');
  if (archive.readUInt16LE(end + 4) || archive.readUInt16LE(end + 6)) {
    throw new Error('Multi-disk ZIP is unsupported');
  }
  const count = archive.readUInt16LE(end + 10);
  const offset = archive.readUInt32LE(end + 16);
  const size = archive.readUInt32LE(end + 12);
  if (count === 65535 || offset === 0xffffffff || size === 0xffffffff ||
      offset + size !== end) throw new Error('ZIP64/invalid central directory');
  const files = new Map();
  let position = offset;
  for (let index = 0; index < count; ++index) {
    if (position + 46 > end || archive.readUInt32LE(position) !== 0x02014b50) {
      throw new Error('Invalid ZIP central entry');
    }
    const nameLength = archive.readUInt16LE(position + 28);
    const extraLength = archive.readUInt16LE(position + 30);
    const commentLength = archive.readUInt16LE(position + 32);
    const next = position + 46 + nameLength + extraLength + commentLength;
    if (next > end) throw new Error('Truncated ZIP central entry');
    const name = archive.toString('utf8', position + 46, position + 46 + nameLength);
    if (!name || name.startsWith('/') || name.includes('\\') ||
        name.split('/').some(part => part === '..') || files.has(name)) {
      throw new Error('Invalid or duplicate ZIP path');
    }
    files.set(name, {
      flags: archive.readUInt16LE(position + 8),
      method: archive.readUInt16LE(position + 10),
      crc: archive.readUInt32LE(position + 16),
      compressed: archive.readUInt32LE(position + 20),
      size: archive.readUInt32LE(position + 24),
      localOffset: archive.readUInt32LE(position + 42),
    });
    position = next;
  }
  if (position !== end) throw new Error('Invalid ZIP directory size');
  return {
    names: [...files.keys()],
    get(name, limit = 8 * 1024 * 1024) {
      const entry = files.get(name);
      if (!entry) return null;
      const local = entry.localOffset;
      if (entry.flags & 1 || entry.size > limit || local + 30 > offset ||
          archive.readUInt32LE(local) !== 0x04034b50) {
        throw new Error(`Unsupported or oversized ZIP entry: ${name}`);
      }
      const start = local + 30 + archive.readUInt16LE(local + 26) +
        archive.readUInt16LE(local + 28);
      if (start + entry.compressed > offset) throw new Error('ZIP entry outside archive');
      const compressed = archive.subarray(start, start + entry.compressed);
      let content;
      if (entry.method === 0) content = compressed;
      else if (entry.method === 8) {
        content = zlib.inflateRawSync(compressed, {maxOutputLength: limit});
      } else throw new Error(`Unsupported ZIP compression method: ${entry.method}`);
      if (content.length !== entry.size || crc32(content) !== entry.crc) {
        throw new Error(`ZIP entry size/CRC mismatch: ${name}`);
      }
      return content;
    },
  };
}

export function makeZip(entries) {
  const local = [];
  const central = [];
  let offset = 0;
  for (const [name, content] of Object.entries(entries)) {
    const filename = Buffer.from(name);
    const data = Buffer.isBuffer(content) ? content : Buffer.from(content);
    const checksum = crc32(data);
    const header = Buffer.alloc(30);
    header.writeUInt32LE(0x04034b50);
    header.writeUInt16LE(20, 4);
    header.writeUInt16LE(0x800, 6);
    header.writeUInt32LE(checksum, 14);
    header.writeUInt32LE(data.length, 18);
    header.writeUInt32LE(data.length, 22);
    header.writeUInt16LE(filename.length, 26);
    local.push(header, filename, data);
    const directory = Buffer.alloc(46);
    directory.writeUInt32LE(0x02014b50);
    directory.writeUInt16LE(20, 4);
    directory.writeUInt16LE(20, 6);
    directory.writeUInt16LE(0x800, 8);
    directory.writeUInt32LE(checksum, 16);
    directory.writeUInt32LE(data.length, 20);
    directory.writeUInt32LE(data.length, 24);
    directory.writeUInt16LE(filename.length, 28);
    directory.writeUInt32LE(offset, 42);
    central.push(directory, filename);
    offset += header.length + filename.length + data.length;
  }
  const directory = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50);
  end.writeUInt16LE(Object.keys(entries).length, 8);
  end.writeUInt16LE(Object.keys(entries).length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...local, directory, end]);
}

export function makeDeveloperCrx(archive, privateKeyPem) {
  const key = crypto.createPrivateKey(privateKeyPem);
  const publicKey = crypto.createPublicKey(key).export({type: 'spki', format: 'der'});
  const idBytes = crypto.createHash('sha256').update(publicKey).digest().subarray(0, 16);
  const signedHeader = protobufField(1, idBytes);
  const signature = crypto.sign('sha256', signedPayload(signedHeader, archive), key);
  const proof = Buffer.concat([protobufField(1, publicKey), protobufField(2, signature)]);
  const header = Buffer.concat([
    protobufField(key.asymmetricKeyType === 'rsa' ? 2 : 3, proof),
    protobufField(10000, signedHeader),
  ]);
  const prefix = Buffer.alloc(12);
  prefix.write('Cr24');
  prefix.writeUInt32LE(3, 4);
  prefix.writeUInt32LE(header.length, 8);
  return Buffer.concat([prefix, header, archive]);
}

export function readCrxFile(filename) {
  if (fs.statSync(filename).size > MAX_CRX_SIZE) throw new Error('CRX exceeds 256 MiB');
  return fs.readFileSync(filename);
}
