#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Development update server (doc/ota.md, "Development procedure"): serves the current application
# build and a manifest.json generated from it over plain HTTP on the desk machine.
#
#   scripts/update-server.py                       # build platform/esp/build-esp32c6, port 8070
#   scripts/update-server.py --port 8080 --build platform/esp/build-esp32s3
#
# Then on the device console:  update now http://<this machine>:8070   (install, whatever version)
#                          or  update url set http://<this machine>:8070 ; update check
#
# The manifest's version is the one the image's app descriptor carries (hc::kVersion of the build),
# so the updater's version check always agrees with it. `update check` installs only a newer
# version: bump kVersion in core/include/hc/version.h for that test; `update now` installs anything.
# The files are copied into <build>/ota-serve/ when the server starts; rebuild and restart the
# server to publish a new build.
#
# Next to the firmware it serves the release's language packs (doc/i18n.md) the way a release
# publishes them: <base>/<version>/i18n/manifest.json and lang-<code>.json.gz, made from web/ui/lang
# by scripts/i18n-pack.py. A device with `update url set <base>` downloads the pack of its ui.lang
# from there (console `lang status`, `lang sync`). --no-i18n leaves them out.
#
# The manifest is signed (doc/ota.md "Manifest signature"): manifest.sig beside it holds the Ed25519
# signature of manifest.json's exact bytes. The device accepts only manifests of the release key its
# firmware carries (HC_RELEASE_KEY at build time, default platform/esp/release-key.pub). By default the
# server signs with a development key, generated on first use into scripts/dev-release-key (git-ignored;
# the public half in scripts/dev-release-key.pub): build the firmware that should take these updates with
#   idf.py -B build-esp32c6 -DHC_RELEASE_KEY=<repo>/scripts/dev-release-key.pub build
# --key <secret-file> signs with another key (the release key, for a release-like test); --unsigned
# serves no manifest.sig (to see the device refuse it). Needs python3-cryptography or python3-nacl
# (scripts/sign.py).

import argparse
import hashlib
import importlib.util
import http.server
import json
import os
import shutil
import socket
import struct
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

ESP_IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 24 + 8  # esp_image_header_t + esp_image_segment_header_t
APP_DESC_MAGIC = 0xABCD5432


def app_descriptor(data):
	"""(project_name, version) from an ESP application image, or a refusal."""
	if len(data) < APP_DESC_OFFSET + 256 or data[0] != ESP_IMAGE_MAGIC:
		raise SystemExit('not an ESP application image')
	magic, = struct.unpack_from('<I', data, APP_DESC_OFFSET)
	if magic != APP_DESC_MAGIC:
		raise SystemExit('the image has no app descriptor')
	version = data[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48].split(b'\0')[0].decode('ascii')
	project = data[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80].split(b'\0')[0].decode('ascii')
	return project, version


def local_address():
	"""The address other hosts on the LAN reach this machine at (no packet is sent)."""
	try:
		with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
			s.connect(('192.0.2.1', 9))  # TEST-NET-1: only selects the outgoing interface
			return s.getsockname()[0]
	except OSError:
		return '<this machine>'


def main():
	parser = argparse.ArgumentParser(description='Serve the application build and its manifest for an OTA update.')
	parser.add_argument('--build', default=os.path.join(REPO, 'platform', 'esp', 'build-esp32c6'),
						help='application build directory (default: platform/esp/build-esp32c6)')
	parser.add_argument('--port', type=int, default=8070, help='HTTP port (default 8070)')
	parser.add_argument('--no-i18n', action='store_true', help='do not serve the language packs')
	parser.add_argument('--key', help='private key file to sign the manifest with (default: the development key '
						'scripts/dev-release-key, generated on first use)')
	parser.add_argument('--unsigned', action='store_true', help='serve the manifest without manifest.sig')
	args = parser.parse_args()

	spec = importlib.util.spec_from_file_location('hc_sign', os.path.join(REPO, 'scripts', 'sign.py'))
	sign = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(sign)
	key = None
	if not args.unsigned:
		key_path = args.key or os.path.join(REPO, 'scripts', 'dev-release-key')
		if args.key is None and not os.path.exists(key_path):
			public = sign.keygen(key_path)
			print('generated the development release key %s (public key %s, fingerprint %s)'
				  % (key_path, public.hex(), sign.fingerprint(public)))
		key = sign.signer(key_path)

	image_path = os.path.join(args.build, 'homecontrol.bin')
	if not os.path.isfile(image_path):
		raise SystemExit('missing %s: build the application first (cd platform/esp && idf.py -B %s build)'
						 % (image_path, os.path.basename(args.build)))
	with open(image_path, 'rb') as f:
		data = f.read()
	project, version = app_descriptor(data)
	if project != 'homecontrol':
		raise SystemExit('%s is "%s", not the application' % (image_path, project))

	serve = os.path.join(args.build, 'ota-serve')
	os.makedirs(serve, exist_ok=True)
	shutil.copyfile(image_path, os.path.join(serve, 'homecontrol.bin'))
	manifest = {
		'version': version,
		'url': 'homecontrol.bin',
		'sha256': hashlib.sha256(data).hexdigest(),
		'size': len(data),
	}
	manifest_bytes = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
	with open(os.path.join(serve, 'manifest.json'), 'wb') as f:
		f.write(manifest_bytes)
	signature_path = os.path.join(serve, 'manifest.sig')
	if key is not None:
		with open(signature_path, 'w') as f:
			f.write(sign.sign_manifest(key, manifest_bytes).hex() + '\n')
	elif os.path.exists(signature_path):
		os.remove(signature_path)

	release = version.split('+')[0]
	i18n_dir = os.path.join(serve, release, 'i18n')
	if os.path.isdir(i18n_dir):
		shutil.rmtree(i18n_dir)
	if not args.no_i18n:
		spec = importlib.util.spec_from_file_location('i18n_pack', os.path.join(REPO, 'scripts', 'i18n-pack.py'))
		i18n_pack = importlib.util.module_from_spec(spec)
		spec.loader.exec_module(i18n_pack)
		if i18n_pack.cmd_manifest(argparse.Namespace(dir=os.path.join(serve, release), version=version)) != 0:
			print('warning: some language packs were refused (above); the others are served')

	base = 'http://%s:%d' % (local_address(), args.port)
	print('serving %s (%s, %d bytes, sha256 %s...) from %s' % (image_path, version, len(data),
															   manifest['sha256'][:16], serve))
	if key is not None:
		print('manifest signed with the key %s (fingerprint %s): the firmware must carry it (HC_RELEASE_KEY)'
			  % (key.public.hex(), sign.fingerprint(key.public)))
		if args.key is None:
			print('  idf.py -B build-esp32c6 -DHC_RELEASE_KEY=%s.pub build' % os.path.join(REPO, 'scripts', 'dev-release-key'))
	else:
		print('manifest NOT signed (--unsigned): the device refuses it')
	print('on the device console:')
	print('  update now %s            install this build, whatever its version' % base)
	print('  update url set %s        then: update check (installs only a newer version)' % base)
	if not args.no_i18n:
		print('language packs: %s/%s/i18n/manifest.json (console: lang sync)' % (base, release))
	print('stop with Ctrl-C')
	sys.stdout.flush()

	handler = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(*a, directory=serve, **kw)
	with http.server.ThreadingHTTPServer(('0.0.0.0', args.port), handler) as server:
		try:
			server.serve_forever()
		except KeyboardInterrupt:
			print('\nstopped')
	return 0


if __name__ == '__main__':
	sys.exit(main())
