# 3. Transitions

You learn: every transition, changing one, its settings and handles, fades, transitions at the
start and end of an event, and removing them.

**Example project:** [transitions](../../data/examples/transitions.mtg)

## What's in it

All 27 transitions in a row, a little under a minute. Animated backgrounds overlap by a second each,
and every overlap has a different transition. The **Names** track says which one is playing.

## 1. Watch them

Press **Space** and watch them all. Each event shows its transition's name on the timeline.

## 2. Change a transition's settings

1. Find **Cube Spin**. Click its name on the event: its settings open in the **Properties** tab.
2. Change its direction, and play it again.
3. Find **Shape Reveal** and change its shape to a heart, its border width and border colour.
4. Find **Ripple**. With its Properties showing, drag its centre to a corner of the preview.
5. Find **Linear Wipe**. Drag the dot on the line on the preview to turn the wipe's angle.

## 3. Swap a transition

1. Open the **Transitions** tab (top left).
2. Drag **Film Burn** onto the overlap of any two events. It replaces the transition there.
3. Or right-click the later event > **Transition In** and pick one from the menu. The menu shows the
   current one.

## 4. Fade curves

Right-click an event > **Fade In Curve** > **Slow**. The transition follows the curve: slow at
first, fast at the end. Try **Smooth** (an S-curve).

## 5. A transition from black

1. Drag **Dip to Color** onto the very start of the first event. With nothing before it, it fades
   from the tracks below (here, black). A 1-second fade is made for it.
2. Drag **Iris** onto the end of the last event: it closes to black.

## 6. Remove one

1. Click the **×** on a transition's label. The overlap becomes a cut where the later event
   starts, and the earlier event is trimmed there.
2. Press **Ctrl+Z** to bring it back.
3. Right-click an event > **Remove Transitions (both ends)** removes both of its transitions at once.

## Next

- [Effects and Keyframes](04-effects-and-keyframes.md)
- [Transitions](../transitions.md) in the guide.
