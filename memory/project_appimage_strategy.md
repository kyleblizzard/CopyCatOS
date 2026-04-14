---
name: AppImage-only distribution — every app bundles breeze-aqua
description: CopiCatOS distributes all apps as AppImages with breeze-aqua baked in. No system dependency can override the look. Each app must be customized/packaged to match Snow Leopard.
type: project
---

Every app in CopiCatOS ships as an AppImage with breeze-aqua (the custom QStyle), Lucida Grande font, color scheme, and Aqua PNG assets bundled inside. This means:

1. No system update can break the styling
2. No app can opt out of the design language
3. Each app IS being customized as part of packaging
4. Apps that can't be Qt6 don't ship — period

**Why:** Kyle's requirement is pixel-perfect without exception. The AppImage strategy is what makes "without exception" achievable — it's the enforcement mechanism. Like macOS .app bundles with AppKit, this is a curated ecosystem.

**How to apply:** When planning the app set for CopiCatOS, every app needs a Qt6 AppImage packaging plan. This includes deciding which Linux apps to use, which to fork, and which to build from scratch. Each AppImage build needs breeze-aqua integration validated.
