#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# The release side of the web UI's language packs (doc/i18n.md, issue #24).
#
#   scripts/i18n-pack.py check                      # every pack against the English base
#   scripts/i18n-pack.py translate [--lang fr]      # fill missing keys with DeepL (DEEPL_AUTH_KEY)
#   scripts/i18n-pack.py manifest <dir> --version 0.2.0
#                                                   # <dir>/i18n/: lang-<code>.json.gz + manifest.json
#
# The base is web/ui/dist/lang/en.json (part of the UI bundle); the packs are web/ui/lang/<code>.json
# with exactly its keys, every value a string with the base's {placeholders}. `check` lists what is
# missing, extra or broken and exits 1 if anything is. `translate` machine-translates the missing
# keys of every pack (or creates and fills the pack of --lang) through the DeepL API when
# DEEPL_AUTH_KEY is set; without it, it lists the missing keys and exits 1 - a pack is always
# reviewed and committed, nothing is translated at release time. `manifest` writes the per-release
# bundle the device downloads from the update server, <update.url>/<version>/i18n/ (the device asks
# for the directory of its own version without build metadata); it refuses packs that fail the
# check or exceed the device's limit.

import argparse
import collections
import gzip
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
BASE = os.path.join(REPO, 'web', 'ui', 'dist', 'lang', 'en.json')
PACKS = os.path.join(REPO, 'web', 'ui', 'lang')

# LanguagePacks::kMaxPack (core/include/hc/language_packs.h): the device's fetch buffer.
MAX_PACK = 12288
TAG = re.compile(r'^[a-z]{2}(-[A-Z]{2})?$')
PLACEHOLDER = re.compile(r'\{(\w+)\}')


def load(path):
	with open(path, encoding='utf-8') as f:
		data = json.load(f, object_pairs_hook=collections.OrderedDict)
	if not isinstance(data, dict):
		raise SystemExit('%s: not a JSON object' % path)
	return data


def save(path, pack, base):
	ordered = collections.OrderedDict((key, pack[key]) for key in base if key in pack)
	with open(path, 'w', encoding='utf-8') as f:
		f.write(json.dumps(ordered, ensure_ascii=False, indent='\t'))
		f.write('\n')


def pack_files():
	"""(code, path) of every committed pack."""
	result = []
	if os.path.isdir(PACKS):
		for name in sorted(os.listdir(PACKS)):
			if name.endswith('.json') and not name.startswith('.'):
				result.append((name[:-5], os.path.join(PACKS, name)))
	return result


def problems(code, pack, base):
	"""The problems of one pack as (kind, key, text); the missing keys are kind 'missing'."""
	found = []
	if not TAG.match(code):
		found.append(('name', '', 'the file name is not a language tag ([a-z]{2}(-[A-Z]{2})?)'))
	for key, text in base.items():
		if key not in pack:
			found.append(('missing', key, text))
			continue
		value = pack[key]
		if not isinstance(value, str) or not value:
			found.append(('broken', key, 'not a non-empty string'))
		elif set(PLACEHOLDER.findall(value)) != set(PLACEHOLDER.findall(text)):
			found.append(('broken', key, 'placeholders %s, the base has %s' % (
				sorted(set(PLACEHOLDER.findall(value))), sorted(set(PLACEHOLDER.findall(text))))))
	for key in pack:
		if key not in base:
			found.append(('extra', key, 'not in en.json'))
	return found


def report(code, found):
	for kind, key, text in found:
		print('%s.json: %-7s %s%s' % (code, kind, key, (' (%s)' % text) if kind != 'missing' else ''))


def cmd_check(args):
	base = load(BASE)
	failed = False
	for code, path in pack_files():
		found = problems(code, load(path), base)
		report(code, found)
		failed = failed or bool(found)
		if not found:
			print('%s.json: complete (%d keys)' % (code, len(base)))
	return 1 if failed else 0


def deepl_target(code):
	return code.upper()


def protect(text):
	"""Placeholders become XML tags DeepL leaves alone."""
	return PLACEHOLDER.sub(lambda m: '<x id="%s"/>' % m.group(1), text.replace('&', '&amp;').replace('<', '&lt;'))


def unprotect(text):
	text = re.sub(r'<x id="(\w+)"\s*/>', r'{\1}', text)
	return text.replace('&lt;', '<').replace('&amp;', '&')


def deepl(key, texts, target):
	host = 'api-free.deepl.com' if key.endswith(':fx') else 'api.deepl.com'
	body = json.dumps({
		'text': [protect(t) for t in texts], 'source_lang': 'EN', 'target_lang': deepl_target(target),
		'tag_handling': 'xml', 'ignore_tags': ['x'], 'preserve_formatting': True,
	}).encode('utf-8')
	request = urllib.request.Request('https://%s/v2/translate' % host, data=body, method='POST', headers={
		'Authorization': 'DeepL-Auth-Key ' + key, 'Content-Type': 'application/json'})
	try:
		with urllib.request.urlopen(request, timeout=60) as response:
			answer = json.load(response)
	except urllib.error.HTTPError as e:
		raise SystemExit('DeepL refused the request: HTTP %d %s' % (e.code, e.read().decode('utf-8', 'replace')[:200]))
	except urllib.error.URLError as e:
		raise SystemExit('DeepL cannot be reached: %s' % e.reason)
	return [unprotect(t['text']) for t in answer['translations']]


def cmd_translate(args):
	base = load(BASE)
	files = pack_files()
	if args.lang:
		if not TAG.match(args.lang) or args.lang == 'en':
			raise SystemExit('--lang %s: a language tag other than en ([a-z]{2}(-[A-Z]{2})?)' % args.lang)
		path = os.path.join(PACKS, args.lang + '.json')
		files = [(args.lang, path)]
	key = os.environ.get('DEEPL_AUTH_KEY', '')
	incomplete = False
	for code, path in files:
		pack = load(path) if os.path.exists(path) else collections.OrderedDict()
		found = problems(code, pack, base)
		missing = [k for kind, k, _ in found if kind == 'missing']
		others = [f for f in found if f[0] != 'missing']
		report(code, others)
		if not missing:
			print('%s.json: nothing missing' % code)
			incomplete = incomplete or bool(others)
			continue
		if not key:
			report(code, [f for f in found if f[0] == 'missing'])
			print('%s.json: %d keys missing; set DEEPL_AUTH_KEY to machine-translate them, or translate them by hand'
				  % (code, len(missing)))
			incomplete = True
			continue
		for start in range(0, len(missing), 50):
			chunk = missing[start:start + 50]
			for k, text in zip(chunk, deepl(key, [base[k] for k in chunk], code)):
				pack[k] = text
		save(path, pack, base)
		print('%s.json: %d keys translated by DeepL - review them before committing' % (code, len(missing)))
		incomplete = incomplete or bool(others)
	return 1 if incomplete else 0


def cmd_manifest(args):
	base = load(BASE)
	release = args.version.split('+')[0]
	out = os.path.join(args.dir, 'i18n')
	os.makedirs(out, exist_ok=True)
	manifest = collections.OrderedDict([('version', release), ('packs', collections.OrderedDict())])
	failed = False
	for code, path in pack_files():
		pack = load(path)
		found = problems(code, pack, base)
		if found:
			report(code, found)
			print('%s.json: refused for the manifest (run: scripts/i18n-pack.py check)' % code)
			failed = True
			continue
		raw = json.dumps(pack, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
		packed = gzip.compress(raw, compresslevel=9, mtime=0)
		if len(packed) > MAX_PACK:
			print('%s.json: refused: %d bytes compressed, the device takes at most %d' % (code, len(packed), MAX_PACK))
			failed = True
			continue
		name = 'lang-%s.json.gz' % code
		with open(os.path.join(out, name), 'wb') as f:
			f.write(packed)
		manifest['packs'][code] = collections.OrderedDict([
			('name', pack['lang.name']), ('path', name), ('sha256', hashlib.sha256(packed).hexdigest()),
			('size', len(packed))])
		print('%s: %s, %d bytes (%d uncompressed)' % (code, name, len(packed), len(raw)))
	with open(os.path.join(out, 'manifest.json'), 'w', encoding='utf-8') as f:
		f.write(json.dumps(manifest, ensure_ascii=False, indent=2))
		f.write('\n')
	print('wrote %s for release %s (%d packs)' % (os.path.join(out, 'manifest.json'), release, len(manifest['packs'])))
	return 1 if failed else 0


def main():
	parser = argparse.ArgumentParser(description='Check, translate and publish the web UI language packs.')
	sub = parser.add_subparsers(dest='command', required=True)
	sub.add_parser('check', help='compare every pack with the English base')
	translate = sub.add_parser('translate', help='fill missing keys through DeepL (DEEPL_AUTH_KEY)')
	translate.add_argument('--lang', help='create or complete only this pack')
	manifest = sub.add_parser('manifest', help='write <dir>/i18n/ for a release')
	manifest.add_argument('dir', help='the release directory on the update server (<update.url>/<version>)')
	manifest.add_argument('--version', required=True, help='the firmware version (hc::kVersion)')
	args = parser.parse_args()
	return {'check': cmd_check, 'translate': cmd_translate, 'manifest': cmd_manifest}[args.command](args)


if __name__ == '__main__':
	sys.exit(main())
