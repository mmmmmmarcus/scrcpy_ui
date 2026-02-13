# ScrcpyUI Figma Bridge Plugin

This plugin receives screenshots from ScrcpyUI's local bridge endpoint and inserts them into the current Figma page.

## Files

- `manifest.json`
- `code.js`
- `ui.html`

## Install in Figma

1. Open Figma desktop app.
2. Plugins -> Development -> Import plugin from manifest...
3. Select `manifest.json` from this folder.
4. Run the plugin: Plugins -> Development -> ScrcpyUI Figma Bridge.

## Use with ScrcpyUI

1. In ScrcpyUI, set screenshot destination to `Figma Bridge` from the settings menu.
2. Keep this plugin UI open (it auto-starts and keeps syncing until closed).
3. Click ScrcpyUI screenshot button.
4. The plugin inserts each new screenshot onto the current canvas.

UI behavior:

- No manual start/stop controls.
- Automatically connects on open and continues until the plugin is closed.

Default endpoint:

`http://127.0.0.1/scrcpy-bridge/latest`

Port fallback (internal, no manual input required):

`http://127.0.0.1:27184/scrcpy-bridge/latest`
