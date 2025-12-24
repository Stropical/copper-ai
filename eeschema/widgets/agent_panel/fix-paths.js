const fs = require('fs');
const path = require('path');

// Fix paths in index.html to work with file:// protocol
const indexPath = path.join(__dirname, 'out', 'index.html');

if (!fs.existsSync(indexPath)) {
  console.error('index.html not found at:', indexPath);
  process.exit(1);
}

let html = fs.readFileSync(indexPath, 'utf8');

// Replace absolute paths with relative paths
// Match href="/_next/ and src="/_next/ patterns
html = html.replace(/href="\/_next\//g, 'href="./_next/');
html = html.replace(/src="\/_next\//g, 'src="./_next/');

// Also fix any other absolute paths that might be in script tags or data attributes
html = html.replace(/"\/_next\//g, '"./_next/');

fs.writeFileSync(indexPath, html, 'utf8');
console.log('✓ Fixed paths in index.html for file:// protocol');

