---
name: Scrollbar failure was Kvantum, not KDE — don't conflate them
description: The Aqua scrollbar issue was caused by Kvantum's rigid SVG composition, not by Qt or KDE. breeze-aqua (custom QStyle in C++) solved it with 3-piece PNG rendering.
type: feedback
---

The Aqua scrollbar problem that nearly derailed the KDE approach was a Kvantum limitation, not a Qt/KDE limitation. Kvantum uses SVGs and has rigid opinions about how scrollbar pieces compose. A custom QStyle (breeze-aqua) gives full QPainter control and solved the problem completely.

**Why:** Kyle almost abandoned KDE over this. The real issue was the theming engine choice (Kvantum), not the toolkit (Qt) or the window manager (kwin). This distinction matters for every future "KDE can't do X" complaint — ask whether it's a toolkit limit or a theming engine limit.

**How to apply:** When a visual limitation surfaces, diagnose whether it's Qt/QStyle level (solvable in breeze-aqua C++), KDE/kwin level, or a third-party theming tool limitation. Don't let Kvantum-class problems drive architectural decisions.
