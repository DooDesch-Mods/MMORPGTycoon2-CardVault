"""Check the mod-specific publication boundaries before any external write."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from package import FILES
from nexus_release import archive_version, checksum_for
from release_notes import section


class ReleaseTests(unittest.TestCase):
    def archive(self, extra=None, identifier='doodesch.cardvault'):
        temporary=tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path=Path(temporary.name)/'mod.zip'
        with zipfile.ZipFile(path,'w') as stream:
            for name in FILES:
                data=json.dumps({'id':identifier,'entry':'MT2CardVault.dll','version':'0.1.0'}) if name=='mod.json' else 'fixture'
                stream.writestr('doodesch.cardvault/'+name,data)
            if extra:stream.writestr(extra,'private fixture')
        return path

    def test_exact_player_archive(self):
        self.assertEqual(archive_version(self.archive(),'cardvault'),'0.1.0')

    def test_private_or_unexpected_file_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'inventory'):
            archive_version(self.archive('doodesch.cardvault/connection.bin'),'cardvault')

    def test_path_escape_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'Unsafe ZIP'):
            archive_version(self.archive('../connection.bin'),'cardvault')

    def test_other_plugin_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'manifest'):
            archive_version(self.archive(identifier='other.mod'),'cardvault')

    def test_changelog_has_exact_release(self):
        version=json.loads((ROOT/'mod.json').read_text())['version']
        self.assertIn('### Added',section((ROOT/'CHANGELOG.md').read_text(),'cardvault',version))

    def test_checksum_must_be_unambiguous(self):
        line='a'*64+'  mod.zip\n'
        with self.assertRaisesRegex(ValueError,'exactly one'):
            checksum_for(line+line,'mod.zip')


if __name__=='__main__':unittest.main()
