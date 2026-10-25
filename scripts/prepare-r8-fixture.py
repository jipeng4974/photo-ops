#!/usr/bin/env python3
"""Register and verify ordered external R8 CR3 fixtures without copying or publishing photos."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile


def read_json(path):
    def invalid(value):
        raise ValueError(f'non-finite JSON constant: {value}')
    raw = path.read_bytes()
    return json.loads(raw, parse_constant=invalid), hashlib.sha256(raw).hexdigest()


def publish(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.photo-ops-fixture-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write((json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n').encode())
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path)  # Atomic no-replace, including an existing dangling symlink.
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


def contained(root, name):
    relative = Path(name)
    if relative.is_absolute() or '..' in relative.parts or not relative.parts:
        raise ValueError(f'expected a scene-relative path without parent traversal: {name}')
    path = (root/relative).resolve(strict=True)
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError(f'file escapes the scene or is not a regular file: {name}')
    return path


def fingerprint(path):
    before = path.stat()
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.stat()
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    if not before.st_size or identity(before) != identity(after):
        raise ValueError(f'empty file or input changed while hashing: {path.name}')
    return digest, after.st_size


def validate_metadata(data, count):
    if not isinstance(data, dict) or data.get('camera') != 'Canon EOS R8':
        raise ValueError('metadata.camera must explicitly be Canon EOS R8')
    for field in ('lens', 'photographer'):
        if not isinstance(data.get(field), str) or not data[field].strip():
            raise ValueError(f'metadata.{field} must be a nonempty string')
    for field in ('focal_length_mm', 'aperture', 'iso', 'exposure_time_seconds'):
        value = data.get(field)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError(f'metadata.{field} must be a finite positive number')
    if data.get('capture') not in ('tripod', 'handheld'):
        raise ValueError('metadata.capture must be tripod or handheld')
    order = data.get('focus_order')
    if not isinstance(order, list) or len(order) != count or not all(isinstance(v, str) and v.strip() for v in order):
        raise ValueError('focus_order must name every frame in the supplied order')
    tags = data.get('scene_tags')
    if not isinstance(tags, list) or not tags or not all(isinstance(v, str) and v.strip() for v in tags):
        raise ValueError('scene_tags must contain at least one explicit fixture purpose')
    for field in ('public', 'canonical_regression'):
        if field in data and not isinstance(data[field], bool):
            raise ValueError(f'metadata.{field} must be a boolean')
    if data.get('public'):
        if not isinstance(data.get('license'), str) or not data['license'].strip():
            raise ValueError('public fixtures require an explicit license')
        if not isinstance(data.get('license_url'), str) or not re.match(r'https?://', data['license_url']):
            raise ValueError('public fixtures require a license_url')
    if data.get('canonical_regression') and (not isinstance(data.get('expected_behavior'), str) or not data['expected_behavior'].strip()):
        raise ValueError('canonical fixtures require an explicit expected_behavior')


def register(root, output, supplied):
    root = root.resolve(strict=True)
    metadata_path = contained(root, 'metadata.json')
    metadata, metadata_hash = read_json(metadata_path)
    if not isinstance(metadata, dict):
        raise ValueError('metadata must be an object')
    declared = metadata.get('files')
    if supplied and declared and list(supplied) != declared:
        raise ValueError('--frames order differs from metadata.files')
    names = supplied or declared
    if not isinstance(names, list) or not 2 <= len(names) <= 256 or not all(isinstance(name, str) for name in names):
        raise ValueError('provide 2..256 ordered CR3 names in metadata.files or --frames')
    validate_metadata(metadata, len(names))
    entries, seen = [], set()
    for index, name in enumerate(names):
        path = contained(root, name)
        if path.suffix.lower() != '.cr3' or path in seen:
            raise ValueError(f'expected unique CR3 frame: {name}')
        seen.add(path)
        digest, size = fingerprint(path)
        entries.append(dict(path=Path(name).as_posix(), sha256=digest, size_bytes=size,
                            group='cr3', focus_position=metadata['focus_order'][index]))
    manifest = dict(version=1, kind='r8-focus-bracket', state='registered-not-decoder-validated',
                    canonical_regression_declared=metadata.get('canonical_regression', False),
                    metadata_file='metadata.json', metadata_sha256=metadata_hash,
                    metadata=metadata, files=entries)
    publish(output or root/'manifest.json', manifest)
    return dict(registered_files=len(entries), manifest=str(output or root/'manifest.json'),
                raw_decoder_validation='not-run', public=metadata.get('public', False))


def verify(manifest_path, directory):
    manifest, _ = read_json(manifest_path)
    if not isinstance(manifest, dict) or type(manifest.get('version')) is not int or manifest['version'] != 1 or manifest.get('kind') != 'r8-focus-bracket':
        raise ValueError('unsupported fixture manifest kind/version')
    root = (directory or manifest_path.parent).resolve(strict=True)
    entries = manifest.get('files')
    if not isinstance(entries, list) or not 2 <= len(entries) <= 256:
        raise ValueError('manifest requires 2..256 ordered files')
    metadata, metadata_hash = read_json(contained(root, manifest.get('metadata_file', 'metadata.json')))
    if metadata_hash != manifest.get('metadata_sha256') or metadata != manifest.get('metadata'):
        raise ValueError('capture metadata changed after registration')
    validate_metadata(metadata, len(entries))
    if manifest.get('canonical_regression_declared') is not metadata.get('canonical_regression', False):
        raise ValueError('canonical designation disagrees with capture metadata')
    if metadata.get('files') and [entry.get('path') for entry in entries if isinstance(entry, dict)] != metadata['files']:
        raise ValueError('frame order disagrees with metadata.files')
    seen = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or not isinstance(entry.get('path'), str):
            raise ValueError('malformed frame entry')
        if entry.get('group') != 'cr3' or type(entry.get('size_bytes')) is not int or entry['size_bytes'] <= 0:
            raise ValueError('invalid frame group or byte size')
        path = contained(root, entry['path'])
        if path.suffix.lower() != '.cr3' or path in seen:
            raise ValueError('duplicate or non-CR3 manifest member')
        seen.add(path)
        if not re.fullmatch(r'[0-9a-f]{64}', str(entry.get('sha256', ''))):
            raise ValueError('malformed SHA-256')
        digest, size = fingerprint(path)
        if digest != entry['sha256'] or size != entry.get('size_bytes'):
            raise ValueError(f'fixture content/size mismatch: {entry["path"]}')
        if entry.get('focus_position') != metadata['focus_order'][index]:
            raise ValueError('frame order disagrees with capture metadata')
    return dict(verified_files=len(entries), camera=metadata['camera'],
                canonical_regression_declared=manifest.get('canonical_regression_declared', False),
                raw_decoder_validation='not-run')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    init = commands.add_parser('init', help='Create a new external capture metadata template')
    init.add_argument('directory', type=Path)
    registration = commands.add_parser('register', help='Hash ordered originals and freeze declared capture metadata')
    registration.add_argument('directory', type=Path)
    registration.add_argument('--output', '-o', type=Path)
    registration.add_argument('--frames', nargs='+')
    verification = commands.add_parser('verify', help='Verify content, order and metadata; does not develop RAW')
    verification.add_argument('manifest', type=Path)
    verification.add_argument('--directory', type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'init':
            args.directory.mkdir(parents=True, exist_ok=True)
            path = args.directory/'metadata.json'
            publish(path, dict(camera='Canon EOS R8', lens='', focal_length_mm=85, aperture=4,
                               iso=100, exposure_time_seconds=.008, capture='tripod', focus_order=['near', 'mid', 'far'],
                               files=[], scene_tags=['focus-breathing'], photographer='',
                               public=False, license='private', canonical_regression=False,
                               expected_behavior='', notes=''))
            result = dict(metadata=str(path), state='template-fill-before-register')
        elif args.command == 'register':
            result = register(args.directory, args.output, args.frames)
        else:
            result = verify(args.manifest, args.directory)
        print(json.dumps(result, ensure_ascii=False))
    except FileExistsError as error:
        print(f'prepare-r8-fixture: output already exists: {error.filename2 or error.filename}', file=sys.stderr)
        return 1
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f'prepare-r8-fixture: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
