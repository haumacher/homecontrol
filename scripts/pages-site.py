#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Builds the GitHub Pages site https://haumacher.github.io/homecontrol/ from the web flasher's pages
# (web/flasher/) and the assets of firmware releases (doc/ota.md "Releases and the web flasher").
# .github/workflows/pages.yml runs it on the downloaded GitHub release assets; scripts/release.sh
# runs it on the staged release alone, as a preview.
#
#   scripts/pages-site.py --out <dir> --release <assets-dir> [--release ...] [--i18n <dir> ...]
#                         [--stable <version>]
#
# --release names the flat asset directory of one release (what release.sh stages and publishes:
# the flash parts, install.json, manifest.json + manifest.sig, i18n-manifest.json and the packs);
# --i18n the directory of an older release of which only the language packs stay on the site. The
# stable release is the newest one without a pre-release suffix, or --stable (a rollback of the
# channel). The site:
#
#   index.html, de/index.html, flasher.css, flasher.js   the flasher pages (web/flasher/)
#   manifest.json                     ESP Web Tools manifest of the stable release (parts below)
#   releases.json                     the releases on the site and the stable one, for the pages
#   firmware/<version>/               flash parts, install.json (ESP Web Tools), manifest.json and
#                                     manifest.sig (the signed update manifest), SHA256SUMS,
#                                     i18n/manifest.json and i18n/lang-<code>.json.gz
#   stable/manifest.json, .sig        the stable release's update manifest, byte for byte
#                                     (update.url = <site>/stable)
#   stable/<version>/i18n/            the language packs of every release on the site (also the
#                                     older ones): the device reads <update.url>/<version>/i18n/

import argparse
import json
import os
import re
import shutil
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
VERSION = re.compile(r'^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$')
PAGE_FILES = ('index.html', 'de/index.html', 'flasher.css', 'flasher.js')
FLASH_PARTS = ('bootloader.bin', 'partition-table.bin', 'hc_updater.bin', 'homecontrol.bin', 'ota_data_ota0.bin',
			   'storage.bin')


def precedence(version):
	"""Sort key by semver precedence: a pre-release before its release."""
	match = VERSION.match(version)
	if not match:
		raise SystemExit('%s is not a release version' % version)
	major, minor, patch, pre = match.groups()
	pre_key = (1,) if pre is None else (0,) + tuple(
		(0, int(p), '') if p.isdigit() else (1, 0, p) for p in pre.split('.'))
	return (int(major), int(minor), int(patch), pre_key)


def release_version(assets, name):
	with open(os.path.join(assets, name), encoding='utf-8') as f:
		version = json.load(f)['version']
	precedence(version)
	return version


def copy_i18n(assets, target):
	os.makedirs(target, exist_ok=True)
	shutil.copyfile(os.path.join(assets, 'i18n-manifest.json'), os.path.join(target, 'manifest.json'))
	for name in sorted(os.listdir(assets)):
		if re.match(r'^lang-[A-Za-z-]+\.json\.gz$', name):
			shutil.copyfile(os.path.join(assets, name), os.path.join(target, name))


def main():
	parser = argparse.ArgumentParser(description='Build the GitHub Pages site of the web flasher and the releases.')
	parser.add_argument('--out', required=True, help='the site directory (created; must not exist)')
	parser.add_argument('--release', action='append', default=[], help='flat asset directory of a release on the site')
	parser.add_argument('--i18n', action='append', default=[],
						help='asset directory of an older release of which only the language packs stay')
	parser.add_argument('--stable', help='the version of the stable channel (default: the newest release)')
	parser.add_argument('--web', default=os.path.join(REPO, 'web', 'flasher'), help='the flasher pages')
	args = parser.parse_args()

	if os.path.exists(args.out):
		raise SystemExit('%s exists' % args.out)
	if not args.release:
		raise SystemExit('no release: the site needs at least one (--release)')

	releases = {}
	for assets in args.release:
		version = release_version(assets, 'manifest.json')
		missing = [n for n in FLASH_PARTS + ('install.json', 'manifest.json', 'manifest.sig', 'i18n-manifest.json')
				   if not os.path.isfile(os.path.join(assets, n))]
		if missing:
			raise SystemExit('release %s (%s) lacks %s' % (version, assets, ', '.join(missing)))
		if release_version(assets, 'i18n-manifest.json') != version:
			raise SystemExit('release %s: its i18n-manifest.json is of another version' % version)
		releases[version] = assets
	older = {}
	for assets in args.i18n:
		version = release_version(assets, 'i18n-manifest.json')
		if version not in releases:
			older[version] = assets

	ordered = sorted(releases, key=precedence, reverse=True)
	if args.stable:
		if args.stable not in releases:
			raise SystemExit('--stable %s is not among the releases on the site (%s)' % (args.stable, ', '.join(ordered)))
		stable = args.stable
	else:
		finals = [v for v in ordered if '-' not in v]
		stable = finals[0] if finals else ordered[0]

	out = args.out
	os.makedirs(out)
	for name in PAGE_FILES:
		target = os.path.join(out, name)
		os.makedirs(os.path.dirname(target), exist_ok=True)
		shutil.copyfile(os.path.join(args.web, name), target)
	open(os.path.join(out, '.nojekyll'), 'w').close()

	for version in ordered:
		assets = releases[version]
		target = os.path.join(out, 'firmware', version)
		os.makedirs(target)
		for name in FLASH_PARTS + ('install.json', 'manifest.json', 'manifest.sig', 'SHA256SUMS'):
			if os.path.isfile(os.path.join(assets, name)):
				shutil.copyfile(os.path.join(assets, name), os.path.join(target, name))
		copy_i18n(assets, os.path.join(target, 'i18n'))
		copy_i18n(assets, os.path.join(out, 'stable', version, 'i18n'))
	for version, assets in older.items():
		copy_i18n(assets, os.path.join(out, 'stable', version, 'i18n'))

	# The stable channel: the signed update manifest exactly as signed (its image URL is absolute in
	# the site's path, so the same bytes serve from stable/ and from firmware/<version>/).
	for name in ('manifest.json', 'manifest.sig'):
		shutil.copyfile(os.path.join(releases[stable], name), os.path.join(out, 'stable', name))

	# The flasher's manifest: the stable release's install.json with its parts in firmware/<version>/.
	with open(os.path.join(releases[stable], 'install.json'), encoding='utf-8') as f:
		install = json.load(f)
	for build in install['builds']:
		for part in build['parts']:
			part['path'] = 'firmware/%s/%s' % (stable, os.path.basename(part['path']))
	with open(os.path.join(out, 'manifest.json'), 'w', encoding='utf-8') as f:
		json.dump(install, f, indent=2)
		f.write('\n')

	with open(os.path.join(out, 'releases.json'), 'w', encoding='utf-8') as f:
		json.dump({'stable': stable, 'releases': [{'version': v, 'prerelease': '-' in v} for v in ordered]}, f, indent=2)
		f.write('\n')

	print('site %s: stable %s, releases %s%s' % (out, stable, ', '.join(ordered),
		  ', language packs of %s' % ', '.join(sorted(older, key=precedence, reverse=True)) if older else ''))
	return 0


if __name__ == '__main__':
	sys.exit(main())
