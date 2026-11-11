"""The reviewed public interface must not silently change on refactoring."""
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[4]

def test_frozen_reflection_manifest():
    binary=ROOT/'build/debug-linux/opcua/opcua-model-manifest'
    actual=json.loads(subprocess.check_output([binary],text=True))
    expected=json.loads((ROOT/'docs/opcua/interface.json').read_text())
    assert actual==expected, 'Regenerate and review the interface manifest before changing the public model'
    ids=[node['id'] for node in actual['nodes']]
    assert len(set(ids))==len(ids)
    assert all('Password' not in identity and 'Credential' not in identity for identity in ids)
    assert 'ProtoMate/System/RemotePermission' in ids
    assert all(node.get('access')=='read' for node in actual['nodes'] if node['kind']=='variable')
