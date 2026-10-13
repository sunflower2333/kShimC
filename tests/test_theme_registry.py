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
        self.assertEqual(matrices, [names, {'METRO', 'ADWAITA', 'HOLO', 'LVGL_DEFAULT', 'CLOVER', 'GLASS', 'SURFACE', 'IOS_HIG'}, names - {'FLAT'}])

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

    def test_unchanged_themes_keep_pixel_comparison_and_clover_has_new_contract(self):
        workflow = (ROOT / '.github/workflows/menu-styles.yml').read_text()
        block = workflow.split('- name: Preserve unchanged original theme pixels\n', 1)[1].split('      - name:', 1)[0]
        # Exactly one intentional layout change, not a broad bypass of CI.
        self.assertEqual(re.findall(r'(?m)^\s+if:\s*(.*)$', block), ["matrix.style != 'CLOVER'"])
        self.assertIn('base=a154e1af2eb65514b15b8f30bb067fb934b6fcad', block)
        self.assertIn('cmp "$file" "previews/$(basename "$file")"', block)
        cmake = (ROOT / 'tests/CMakeLists.txt').read_text()
        self.assertIn('kshim_test(menu_clover_picker_test', cmake)
        source = (ROOT / 'tests/menu_clover_picker_test.c').read_text()
        self.assertIn('ui.BootButton==NULL', source)
        self.assertIn('focus is not activation', source)
        self.assertIn('scrolling must never activate', source)


class PowerPresentationRegistryTest(unittest.TestCase):
    def test_every_design_family_has_an_explicit_power_profile(self):
        design = (ROOT / 'src/ui/menu_design.h').read_text()
        profile = (ROOT / 'src/ui/menu_power_profile.h').read_text()
        kinds = set(re.findall(r'\b(KSHIM_DESIGN_[A-Z0-9_]+)=\d+', design))
        cases = set(re.findall(r'case (KSHIM_DESIGN_[A-Z0-9_]+):', profile))
        self.assertEqual(kinds, cases)
        self.assertIn('case 0:', profile)  # FLAT is deliberately not a custom descriptor.
        self.assertIn('default: return (kshim_power_profile_t){0}', profile)

    def test_framebuffer_bottom_bar_is_not_a_fallback(self):
        port = (ROOT / 'src/ui/lvgl_port.c').read_text()
        power = (ROOT / 'src/ui/menu_power.h').read_text()
        self.assertIn('KshimPowerReserve(void) { return 0; }', port)
        self.assertNotIn('(w-3*gap)/2', power)
        self.assertNotIn('h-reserve+gap', power)
        self.assertNotIn('960U', power)
        self.assertIn('KshimPowerExtendedReattach()', power)
        self.assertIn('KshimPowerExtendedDialog(action)', power)

if __name__ == '__main__':
    unittest.main()
