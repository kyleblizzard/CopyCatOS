---
name: CopyCatOS is a closed ecosystem, not a themed Linux
description: CopyCatOS ships only Qt6 AppImages styled by breeze-aqua — no unstyled apps allowed. This is the macOS philosophy applied to Linux.
type: project
---

CopyCatOS is not "Linux with a Snow Leopard theme." It is a closed, curated OS where every app is a Qt6 AppImage bundled with breeze-aqua styling. Apps that can't look pixel-perfect Snow Leopard don't ship.

**Why:** Kyle's explicit requirement is pixel-perfect Snow Leopard "without exception." No GTK apps, no Electron apps with mismatched scrollbars, no visual compromises. If an app can't be made to look right, it gets replaced with one that can.

**How to apply:** Never suggest GTK alternatives, never suggest "it's close enough." Every app recommendation must be Qt6-native or rebuilt as Qt6. breeze-aqua (the custom QStyle) is the AppKit equivalent — it enforces the design language across the entire ecosystem. When discussing app choices, filter through this lens first.
