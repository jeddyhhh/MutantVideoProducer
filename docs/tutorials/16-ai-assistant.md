# 16. Letting an AI Assistant Edit

You learn: connecting an AI assistant to Mutant Video Producer through MCP, what to ask it, and how to undo what it does.

**Example project:** any. [first-edit](../../data/examples/first-edit.mvp) is a good start.

You need an AI assistant that supports MCP, such as
[Claude Code](https://claude.com/claude-code) or Claude Desktop.

## 1. Turn it on

1. **Options > Allow AI Control (MCP)**.
2. **Options > AI Control Setup...** shows the lines for your computer.

## 2. Connect Claude Code

In a terminal, run the line from the setup window:

```
claude mcp add --transport http mvp http://127.0.0.1:47811/mcp
```

Start Claude Code. It now has Mutant Video Producer's tools.

## 3. Or connect Claude Desktop

Add the lines from the setup window to `claude_desktop_config.json`, and restart Claude Desktop.
They start `MutantVideoProducer.exe --mcp-stdio`, which passes messages on to the Mutant Video Producer you have open.

## 4. Ask it things

With the example open, try:

- *"What's in this project?"*
- *"Add a Glow to the aurora event and make it pulse from 0 to 2 over its length."*
- *"Put a Cube Spin transition between the aurora and the starfield."*
- *"Find the beats in the music and put a marker on every bar."*
- *"Look at the frame at 3 seconds and tell me whether the title is readable."*
- *"Add captions to the narration"* (in the audio-mixing example).
- *"Render this as a TikTok video to my Videos folder."*
- On the Live tab: *"Fill the empty pads with Medium random chains and set the tempo to 124."*

Watch the timeline and the preview change as it works. The status bar shows each tool it uses.

## 5. Undo

Every edit the assistant makes is one *AI: ...* step in the **History** tab. Press **Ctrl+Z** to
undo any of them, or click an earlier step in History.

## Safety

- Only programs on this computer can connect. Requests from web pages are refused.
- It cannot go live on a streaming site: that's always your decision.
- Untick **Options > Allow AI Control (MCP)** to turn it off.

## Next

- [AI Control (MCP)](../ai-control.md) in the guide lists every tool.
- Back to the [Tutorials](README.md).
