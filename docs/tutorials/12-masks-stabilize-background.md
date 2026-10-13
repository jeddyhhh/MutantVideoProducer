# 12. Masks, Stabilising and Removing Backgrounds

You learn: freehand masks and rotoscoping, Track Motion, Pan/Crop, Stabilize, and Remove Background (AI).

**Example project:** [effects-and-keyframes](../../data/examples/effects-and-keyframes.mvp) for the
masks. Stabilize and Remove Background need footage of your own: a handheld clip, and a clip (or
photo) of a person.

## 1. A freehand mask

1. Open the example and click the second event (from 12.5 seconds).
2. On the Pixelate device, set the mask's **Shape** to **Freehand**. The outline starts round, with 6 points.
3. Drag the points on the preview into a shape around the ball.
4. **Alt+click** the outline to add a point. **Alt+click** a point to remove it.
5. Untick **Smooth curve** for straight lines between the points, then tick it again.
6. Drag inside the shape to move all of it.

## 2. Track it

1. Put the cursor at the start of the event and click **Track Motion**.
2. Play: the freehand shape follows the ball. Its points stay put inside it, as they sit in the
   mask's own frame.

## 3. Rotoscope

When the thing you're masking changes shape:

1. Tick **Animate shape**.
2. Put the cursor somewhere, and drag points to fit the shape there: they are keyed at the cursor.
3. Move the cursor a second on and adjust the points again. Repeat.
4. Play: the outline changes shape between the keys.

## 4. Pan/Crop

1. Click the first event and press **Ctrl+Shift+P**. **Pan/Crop** opens in the Properties tab.
2. Scroll on the preview to zoom in, and drag the picture to pan.
3. At the event's start, click **◇** for the zoom and the centre. Move to its end, zoom further and
   pan elsewhere: a slow Ken Burns move.

## 5. Stabilize (your own footage)

1. Import a handheld clip and place it.
2. Right-click it > **Stabilize...**.
3. Choose **Medium** and keep **Zoom in to hide the moving edges** ticked.
4. Click OK. The footage is analysed once (it takes a moment), then plays steadied.
5. Try **Very strong (nearly a tripod)**. Choose **Off** to remove it.

## 6. Remove Background (your own footage)

1. Import a clip or photo of a person and place it on a video track. Put something else on the track
   below it: another clip, or an animated background from the Generators tab.
2. Drag **Remove Background (AI)** (Video FX > Keying & Compositing) onto the person.
3. **Background**: *Transparent*. The track below shows through around the person.
4. Switch **Model** to *Quality (video matting)*: the edge follows hair more closely.
5. Try **Background**: *Blurred*, for a portrait look, or *Color* for a plain backdrop.
6. Tick **Remove the person instead** to keep the room and lose the person.

It runs on the graphics card through DirectML on any DirectX 12 card, or on the CPU without one.
It also works on the Live tab, on a webcam.

## Next

- [Rendering and Sharing](13-rendering.md)
- [Effects](../effects.md#masks) and [Video Effects](../video-effects.md#remove-background-ai) in the guide.
