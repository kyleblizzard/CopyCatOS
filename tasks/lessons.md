# AuraOS Lessons Learned

Patterns and rules to prevent repeated mistakes.

## Architecture
- **Kvantum was the problem, not KDE.** When widget rendering breaks, check the QStyle layer first — not the WM or DE. The scrollbar rendering failure was Kvantum fighting with custom artwork, solved by forking Breeze into breeze-aqua.
- **Don't let frustration with one layer drive changes to an unrelated layer.** WM handles windows; QStyle handles widgets. They are independent.

## Approach
- **No patches, no compromises.** Always rebuild from scratch with real assets rather than patching existing themes. The goal is a perfect copy.
- **Use real Snow Leopard assets from SnowReverseOutput/.** These are HITheme-rendered, pixel-perfect. Don't hand-draw what already exists.
