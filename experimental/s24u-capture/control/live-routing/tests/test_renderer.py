"""Host-only synthetic XML semantics; no vendor templates or device access."""
from pathlib import Path
import itertools
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
STREAMS = ['music', 'voice_call', 'ring', 'alarm', 'notification', 'system']

def normalized(node):
    return (node.tag, sorted(node.attrib.items()), (node.text or '').strip(),
            [normalized(child) for child in node])

def stock(root):
    return {s.attrib['type']: [normalized(a) for a in s if a.attrib.get('effect') != 'clarifae_out']
            for s in root.findall('./postprocess/stream')
            if any(a.attrib.get('effect') != 'clarifae_out' for a in s)}

def render(text, targets):
    return subprocess.run(['awk', '-v', 'targets=' + targets, '-f', str(ROOT/'render-targets.awk')],
                          input=text, text=True, capture_output=True)

cases = 0
for fixture in sorted((ROOT/'tests/fixtures').glob('*.xml')):
    before_text = fixture.read_text()
    before = ET.fromstring(before_text)
    for mask in itertools.product([False, True], repeat=6):
        selected = [s for s, on in zip(STREAMS, mask) if on]
        result = render(before_text, ','.join(selected))
        assert result.returncode == 0, result.stderr
        after = ET.fromstring(result.stdout)
        applies = [s.attrib['type'] for s in after.findall('./postprocess/stream')
                   for a in s.findall('apply') if a.attrib.get('effect') == 'clarifae_out']
        assert sorted(applies) == sorted(selected), (fixture, selected, applies)
        assert stock(before) == stock(after)
        for section in ['libraries', 'effects', 'preprocess']:
            assert normalized(before.find(section)) == normalized(after.find(section))
        again = render(result.stdout, ','.join(selected))
        assert again.returncode == 0 and normalized(ET.fromstring(again.stdout)) == normalized(after)
        cases += 1

minimal = (ROOT/'tests/fixtures/generic.xml').read_text()
negative = [
    (minimal, 'bad'), (minimal, 'music,music'), (minimal, ',music'),
    (minimal, 'music,'), (minimal, 'music,,voice_call'),
    (minimal, 'music' + chr(10) + 'bad'),
    (minimal.replace('</postprocess>', ''), 'music'),
    (minimal.replace('<postprocess>', ''), 'music'),
    (minimal.replace('<stream type="music">', '<stream type="music"><apply effect="fake_output"/>'), 'music'),
    (minimal.replace('<apply effect="clarifae_out"/>', '<apply effect="clarifae_out"/><apply effect="fake_output"/>'), 'music'),
    (minimal.replace('<stream type="voice_call">', '<stream type="music">'), 'music'),
]
for text, targets in negative:
    assert render(text, targets).returncode != 0, targets
print(f'PASS renderer semantic_cases={cases} negative_cases={len(negative)}; synthetic minimal XML only')
