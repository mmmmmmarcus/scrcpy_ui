figma.showUI(__html__, {
  width: 288,
  height: 334,
  themeColors: true,
});

function clampDimension(value, fallback) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return fallback;
  }
  const rounded = Math.round(value);
  return Math.max(1, rounded);
}

function insertScreenshot(bytes, width, height) {
  const image = figma.createImage(bytes);

  const node = figma.createRectangle();
  node.resize(clampDimension(width, 100), clampDimension(height, 100));
  node.fills = [
    {
      type: 'IMAGE',
      imageHash: image.hash,
      scaleMode: 'FILL',
    },
  ];

  const center = figma.viewport.center;
  node.x = center.x - node.width / 2;
  node.y = center.y - node.height / 2;

  figma.currentPage.appendChild(node);
  figma.currentPage.selection = [node];
  figma.viewport.scrollAndZoomIntoView([node]);

  return node;
}

figma.ui.onmessage = (msg) => {
  if (!msg || msg.type !== 'insert-screenshot') {
    return;
  }

  try {
    if (!Array.isArray(msg.bytes) || !msg.bytes.length) {
      throw new Error('Empty PNG data');
    }

    const bytes = new Uint8Array(msg.bytes);
    const node = insertScreenshot(bytes, msg.width, msg.height);
    figma.notify(`Inserted screenshot ${node.width}x${node.height}`);

    figma.ui.postMessage({
      type: 'inserted',
      seq: msg.seq || 0,
      width: node.width,
      height: node.height,
    });
  } catch (e) {
    figma.notify(`Insert failed: ${e.message}`, { error: true });
    figma.ui.postMessage({
      type: 'insert-error',
      message: String(e.message || e),
    });
  }
};
