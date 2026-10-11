"""Keep selectable themes synchronized across firmware, host tests and previews."""
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class ThemeRegistryTest(unittest.TestCase):
    def test_supported_presets(self):
        names = load('test_menu_styles').STYLES
        self.assertEqual(len(names), 29)
        self.assertEqual(len(set(names)), 29)
        self.assertNotIn('ONE_UI', names)
        self.assertTrue({'FLAT', 'SURFACE', 'SURFACE_DARK', 'IOS_HIG', 'LVGL_DEFAULT'} <= set(names))

    def test_all_registries_agree(self):
        names = set(load('test_menu_styles').STYLES)
        generator = load('generate_design_presets')
        self.assertEqual(names, {'FLAT'} | set(generator.ORDER))
        self.assertEqual(names, set(load('build_theme_review').THEMES))
        configs = [ROOT / 'src/Kconfig', *sorted((ROOT / 'src/ui/styles').glob('*Kconfig'))]
        symbols = set()
        for path in configs:
            symbols.update(re.findall(r'^\s*config KSHIM_MENU_STYLE_([A-Z0-9_]+)\s*$', path.read_text(), re.M))
        self.assertEqual(names, symbols)
        cmake = (ROOT / 'tests/CMakeLists.txt').read_text()
        choices = re.search(r'set\(KSHIM_TEST_MENU_STYLES\s+([^)]*)\)', cmake)
        self.assertIsNotNone(choices)
        self.assertEqual(names, set(choices[1].split()))
        workflow = (ROOT / '.github/workflows/menu-styles.yml').read_text()
        matrices = [set(s.replace(',', ' ').split()) for s in re.findall(r'^\s+style: \[([^]]+)\]', workflow, re.M)]
        self.assertEqual(matrices, [names, {'METRO', 'ADWAITA', 'HOLO', 'LVGL_DEFAULT'}, names - {'FLAT'}])

    def test_retired_implementation_removed(self):
        self.assertFalse((ROOT / 'src/ui/styles/one_ui.h').exists())
        self.assertFalse((ROOT / 'tests/menu_one_ui_identity_test.c').exists())
        for path in (ROOT / 'src/ui').rglob('*'):
            if path.is_file() and path.suffix in ('.c', '.h'):
                self.assertNotIn('KSHIM_DESIGN_ONE_UI', path.read_text(), str(path))
                self.assertNotIn('CONFIG_KSHIM_MENU_STYLE_ONE_UI', path.read_text(), str(path))

    def test_generated_header_and_stable_kind_ids(self):
        header = (ROOT / 'src/ui/menu_design.h').read_text()
        self.assertEqual(header, load('generate_design_presets').generate())
        for name, value in {'SURFACE': 10, 'IOS_HIG': 12, 'METRO': 21, 'ADWAITA': 23, 'HOLO': 24, 'LVGL_DEFAULT': 25}.items():
            self.assertRegex(header, rf'\bKSHIM_DESIGN_{name}={value}\b')

    def test_remaining_themes_all_get_pixel_comparison(self):
        workflow = (ROOT / '.github/workflows/menu-styles.yml').read_text()
        block = workflow.split('- name: Preserve every original theme pixel for pixel\n', 1)[1].split('      - name:', 1)[0]
        self.assertNotRegex(block, r'(?m)^\s+if:')
        self.assertIn('base=a154e1af2eb65514b15b8f30bb067fb934b6fcad', block)

if __name__ == '__main__':
    unittest.main()
