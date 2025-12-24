# Agent Panel Chat Interface

A simple chat window built with Next.js, React, and shadcn/ui AI elements for the KiCad schematic editor.

## Setup

1. Install dependencies:
```bash
npm install
# or
pnpm install
# or
yarn install
```

2. Add shadcn AI elements (if available):
```bash
npx shadcn@latest add @ai-elements/all --yes
```

Note: If `@ai-elements/all` doesn't work, you may need to add specific AI components individually. The chat interface is already functional without AI-specific components - you can customize it with your own AI integration.

3. Run the development server:
```bash
npm run dev
# or
pnpm dev
# or
yarn dev
```

4. Open [http://localhost:3000](http://localhost:3000) in your browser.

## Building for Production

To build a standalone production bundle:

```bash
npm run build
npm start
```

## Integration with KiCad WebView

This application can be loaded into the KiCad WebView panel. After building, you can:

1. Serve the built files from a local server
2. Or load the HTML directly into the WebView using `SetPage()` or `LoadURL()`

## Customization

The chat interface is located in `components/chat-window.tsx`. You can:

- Connect it to your AI API by modifying the `handleSend` function
- Customize the styling in `app/globals.css`
- Add additional features like file uploads, code blocks, etc.

## Project Structure

```
agent_panel/
├── app/
│   ├── layout.tsx      # Root layout
│   ├── page.tsx        # Main page
│   └── globals.css     # Global styles
├── components/
│   ├── ui/             # shadcn/ui components
│   └── chat-window.tsx # Main chat component
├── lib/
│   └── utils.ts        # Utility functions
└── package.json        # Dependencies
```

