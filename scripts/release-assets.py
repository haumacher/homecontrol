#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Assembles and signs the assets of a firmware release (doc/ota.md "Releases and the web flasher").
# Shared by the release workflow (.github/workflows/release.yml) and the local check
# scripts/release.sh, so that both produce the same files.
#
#   scripts/release-assets.py assemble --app <app build> --updater <updater build> --version <X.Y.Z> --out <dir>
#   scripts/release-assets.py sign --key <secret-file> --out <dir> [--public <file>] [--site <dir>]
#
# assemble checks the builds (the release key the application carries, no HC_EXTRA_CA, no updater
# test option, the application's version) and writes into <dir>: the six flash parts, install.json
# (the ESP Web Tools manifest, from web/flasher/manifest.json, its offsets checked against the
# build's flasher_args.json), the update manifest manifest.json, the language packs
# (i18n-manifest.json, lang-<code>.json.gz) and the ELF files. sign writes manifest.sig with the
# secret key (scripts/sign.py), refuses unless it verifies against --public (default the committed
# platform/esp/release-key.pub), writes SHA256SUMS and, with --site, the Pages site of this release
# alone (scripts/pages-site.py).

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
RELEASE_KEY_PUB = os.path.join(REPO, 'platform', 'esp', 'release-key.pub')
FLASHER_TEMPLATE = os.path.join(REPO, 'web', 'flasher', 'manifest.json')
# Where the site serves a release's image: the signed manifest names it by this path (a leading /
# keeps the scheme and host of update.url, doc/ota.md "The manifest").
SITE_PATH = '/homecontrol/firmware/%s/homecontrol.bin'
MAX_MANIFEST = 1024


def fail(message):
	raise SystemExit('release-assets: ' + message)


def read(path, mode='r'):
	with open(path, mode) as f:
		return f.read()


def sdkconfig(build):
	values = {}
	for line in read(os.path.join(build, 'sdkconfig')).splitlines():
		if line.startswith('CONFIG_') and '=' in line:
			key, value = line.split('=', 1)
			values[key] = value
	return values


def check_builds(app, updater):
	if sdkconfig(app).get('CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE') == 'y':
		fail('the application trusts extra CA certificates (HC_EXTRA_CA): not a release build')
	if sdkconfig(updater).get('CONFIG_HC_UPDATER_TEST_BUTTON_WINDOW') == 'y':
		fail('the updater is a test build (HC_UPDATER_TEST_BUTTON_WINDOW)')
	for build in (app, updater):
		if sdkconfig(build).get('CONFIG_ESPTOOLPY_FLASHSIZE') != '"4MB"':
			fail('%s is not built for the 4 MB layout' % build)
	header = read(os.path.join(app, 'hc_generated', 'hc_release_key.h'))
	key = read(RELEASE_KEY_PUB).strip()
	if '"%s"' % key not in header:
		fail('the application does not carry the release key of platform/esp/release-key.pub')


def app_descriptor(path):
	data = read(path, 'rb')[:512]
	desc = 24 + 8  # esp_image_header_t + esp_image_segment_header_t
	if len(data) < desc + 80 or data[0] != 0xE9 or struct.unpack_from('<I', data, desc)[0] != 0xABCD5432:
		fail('%s has no app descriptor' % path)
	version = data[desc + 16:desc + 48].split(b'\0')[0].decode('ascii')
	project = data[desc + 48:desc + 80].split(b'\0')[0].decode('ascii')
	return project, version


def install_manifest(app, version, out):
	flash = json.loads(read(os.path.join(app, 'flasher_args.json')))['flash_files']
	by_name = {os.path.basename(path): int(offset, 16) for offset, path in flash.items()}
	manifest = json.loads(read(FLASHER_TEMPLATE))
	parts = manifest['builds'][0]['parts']
	names = [os.path.basename(p['path']) for p in parts]
	for part, name in zip(parts, names):
		if by_name.get(name) != part['offset']:
			fail('install.json: %s at %s, the build flashes it at %s' % (
				name, hex(part['offset']), hex(by_name[name]) if name in by_name else 'no offset'))
		part['path'] = name
	missing = set(by_name) - set(names)
	if missing:
		fail('install.json lacks %s (web/flasher/manifest.json)' % ', '.join(sorted(missing)))
	manifest.pop('_note', None)
	manifest['version'] = version
	with open(os.path.join(out, 'install.json'), 'w') as f:
		json.dump(manifest, f, indent=2)
		f.write('\n')
	return names


def cmd_assemble(args):
	app, updater, out, version = args.app, args.updater, args.out, args.version
	check_builds(app, updater)
	os.makedirs(out, exist_ok=True)
	sources = {
		'bootloader.bin': os.path.join(app, 'bootloader', 'bootloader.bin'),
		'partition-table.bin': os.path.join(app, 'partition_table', 'partition-table.bin'),
		'homecontrol.bin': os.path.join(app, 'homecontrol.bin'),
		'ota_data_ota0.bin': os.path.join(app, 'ota_data_ota0.bin'),
		'storage.bin': os.path.join(app, 'storage.bin'),
		'homecontrol.elf': os.path.join(app, 'homecontrol.elf'),
		'hc_updater.bin': os.path.join(updater, 'hc_updater.bin'),
		'hc_updater.elf': os.path.join(updater, 'hc_updater.elf'),
	}
	for name, source in sources.items():
		shutil.copyfile(source, os.path.join(out, name))

	project, app_version = app_descriptor(os.path.join(out, 'homecontrol.bin'))
	if project != 'homecontrol' or app_version != version:
		fail('homecontrol.bin is %s %s, the release is homecontrol %s' % (project, app_version, version))
	install_manifest(app, version, out)

	image = read(os.path.join(out, 'homecontrol.bin'), 'rb')
	manifest = {'version': version, 'url': SITE_PATH % version, 'sha256': hashlib.sha256(image).hexdigest(),
				'size': len(image)}
	text = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
	if len(text) > MAX_MANIFEST:
		fail('manifest.json is %d bytes, the device reads at most %d' % (len(text), MAX_MANIFEST))
	with open(os.path.join(out, 'manifest.json'), 'wb') as f:
		f.write(text)

	with tempfile.TemporaryDirectory() as stage:
		subprocess.run([sys.executable, os.path.join(REPO, 'scripts', 'i18n-pack.py'), 'manifest', stage,
						'--version', version], check=True)
		i18n = os.path.join(stage, 'i18n')
		shutil.copyfile(os.path.join(i18n, 'manifest.json'), os.path.join(out, 'i18n-manifest.json'))
		for name in sorted(os.listdir(i18n)):
			if name.startswith('lang-'):
				shutil.copyfile(os.path.join(i18n, name), os.path.join(out, name))
	print('assembled release %s in %s (unsigned)' % (version, out))
	return 0


def verify(public_file, manifest, signature):
	public = bytes.fromhex(read(public_file).strip())
	message = read(manifest, 'rb')
	sig = bytes.fromhex(read(signature).strip())
	try:
		from cryptography.exceptions import InvalidSignature
		from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
		try:
			Ed25519PublicKey.from_public_bytes(public).verify(sig, message)
			return True
		except InvalidSignature:
			return False
	except ImportError:
		import nacl.exceptions
		import nacl.signing
		try:
			nacl.signing.VerifyKey(public).verify(message, sig)
			return True
		except nacl.exceptions.BadSignatureError:
			return False


def cmd_sign(args):
	out = args.out
	manifest = os.path.join(out, 'manifest.json')
	signature = os.path.join(out, 'manifest.sig')
	subprocess.run([sys.executable, os.path.join(REPO, 'scripts', 'sign.py'), 'manifest', args.key, manifest], check=True)
	if not verify(args.public, manifest, signature):
		fail('manifest.sig does not verify against %s: the secret is not its key' % args.public)
	print('manifest.sig verifies against %s' % args.public)
	sums = []
	for name in sorted(os.listdir(out)):
		path = os.path.join(out, name)
		if os.path.isfile(path) and name != 'SHA256SUMS':
			sums.append('%s  %s\n' % (hashlib.sha256(read(path, 'rb')).hexdigest(), name))
	with open(os.path.join(out, 'SHA256SUMS'), 'w') as f:
		f.writelines(sums)
	if args.site:
		if os.path.exists(args.site):
			shutil.rmtree(args.site)
		subprocess.run([sys.executable, os.path.join(REPO, 'scripts', 'pages-site.py'), '--out', args.site,
						'--release', out], check=True)
	return 0


def main():
	parser = argparse.ArgumentParser(description='Assemble and sign the assets of a firmware release.')
	sub = parser.add_subparsers(dest='command', required=True)
	assemble = sub.add_parser('assemble', help='copy and check the build outputs, write the manifests and packs')
	assemble.add_argument('--app', required=True, help='the application build directory')
	assemble.add_argument('--updater', required=True, help='the updater build directory')
	assemble.add_argument('--version', required=True, help='the release version (hc::kVersion)')
	assemble.add_argument('--out', required=True, help='the asset directory')
	sign = sub.add_parser('sign', help='sign manifest.json, verify it, write SHA256SUMS')
	sign.add_argument('--key', required=True, help='the secret key file (scripts/sign.py format)')
	sign.add_argument('--out', required=True, help='the asset directory')
	sign.add_argument('--public', default=RELEASE_KEY_PUB, help='the public key the signature must verify against')
	sign.add_argument('--site', help='also build the Pages site of this release into this directory')
	args = parser.parse_args()
	return {'assemble': cmd_assemble, 'sign': cmd_sign}[args.command](args)


if __name__ == '__main__':
	sys.exit(main())
