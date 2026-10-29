# SPDX-License-Identifier: Apache-2.0
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from release_bundle import ROOT, read_json, verify_bundle, verify_signature

class ReleaseValidationTests(unittest.TestCase):
    def test_duplicate_json_rejected(self):
        with self.assertRaises(ValueError):
            read_json('{"sequence":36,"sequence":1}')

    def test_original_signature(self):
        folder=Path(__file__).parent/'fixtures'
        verify_signature({n:(folder/n).read_bytes() for n in ['manifest.json','manifest.sig']},ROOT/'trust/distribution.public.pem')

    def test_modified_signed_manifest_rejected(self):
        folder=Path(__file__).parent/'fixtures'
        with self.assertRaisesRegex(ValueError,'signature verification failed'):
            verify_signature({'manifest.json':(folder/'manifest.json').read_bytes()+b' ', 'manifest.sig':(folder/'manifest.sig').read_bytes()},ROOT/'trust/distribution.public.pem')

    def test_modified_key_rejected_before_crypto(self):
        with tempfile.TemporaryDirectory() as tmp:
            key=Path(tmp)/'key.pem';key.write_bytes((ROOT/'trust/distribution.public.pem').read_bytes()+b'\n')
            with self.assertRaisesRegex(ValueError,'trust pin'):
                verify_signature({},key)

    def archive(self, path, members):
        with tarfile.open(path,'w:gz') as t:
            for name,kind,size in members:
                member=tarfile.TarInfo(name);member.type=kind;member.size=size
                if kind==tarfile.SYMTYPE:
                    member.linkname='../outside';member.size=0
                t.addfile(member,io.BytesIO(b'x'*size) if kind==tarfile.REGTYPE else None)

    def test_hostile_archives_cleanup_without_escape(self):
        for entries in [
            [('../outside',tarfile.REGTYPE,1)],
            [('system.img',tarfile.SYMTYPE,0)],
            [('manifest.json',tarfile.REGTYPE,1),('manifest.json',tarfile.REGTYPE,1)],
            [('manifest.sig',tarfile.REGTYPE,513)],
            [('boot.img',tarfile.REGTYPE,0)],
            [('unexpected.txt',tarfile.REGTYPE,1)],
        ]:
            with self.subTest(entries=entries),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);archive=root/'bad.tar.gz';dest=root/'unpacked'
                self.archive(archive,entries)
                with self.assertRaises(ValueError):verify_bundle(archive,dest)
                self.assertFalse(dest.exists());self.assertFalse((root/'outside').exists())

    def test_existing_destination_is_never_changed(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest=Path(tmp)/'existing';dest.mkdir();(dest/'keep').write_text('keep')
            with self.assertRaises(FileExistsError):verify_bundle(Path(tmp)/'absent',dest)
            self.assertEqual((dest/'keep').read_text(),'keep')

    def test_missing_archive_cleans_new_destination(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest=Path(tmp)/'new'
            with self.assertRaises(FileNotFoundError):verify_bundle(Path(tmp)/'absent',dest)
            self.assertFalse(dest.exists())

if __name__=='__main__':unittest.main()
