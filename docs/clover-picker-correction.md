# Clover picker correction

This local review changes the Clover preset only: the independent Boot control
is removed, not merely hidden. Its public `BootButton` handle is NULL; other
presets retain their previous control and activation behavior.

The main arrangement is boot-media icons, a small system-utility row, then the
selected-entry description. Restart/Power off are utility icons, not fake boot
entries and not full-screen footer buttons. Empty tool space is not a Boot target.

Touch clicks and Enter on a boot entry activate it directly. Focus navigation,
press-down, cancelled touches and scrolling do not enqueue a boot. Power dialogs
retain isolated input groups, Cancel-first focus, explicit execution confirmation
and platform capability checks. The Qualcomm backend is not changed.

The original Clover menu source likewise separates ActionSelect from ActionEnter
and keyboard navigation from entry activation:
https://github.com/CloverHackyColor/CloverBootloader/blob/master/rEFIt_UEFI/gui/REFIT_MENU_SCREEN.cpp

This is an embedded touch adaptation, not a claim that original Clover always
single-clicks to boot. It reproduces the two-row organization from the supplied
reference, not its logo, icon assets, exact colors or operating-system branding.

The historical pixel comparison deliberately excludes Clover because its layout
has intentionally changed. Its new dedicated host test checks actual widgets,
input events, 49-entry rebuilding, modal isolation, and absence of a Boot object.
Other themes still retain their exact-pixel baseline check in CI.

Local validation and source hashes are supplied in the review package. Local
checks do not imply a GitHub commit, CI run or physical-device test.
