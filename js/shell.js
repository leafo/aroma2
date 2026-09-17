import { initAroma } from './aroma.js';
import { compileMoonScript } from './moonscript.js';

// Get DOM elements
const canvas = document.getElementById('canvas');
const codeEditor = document.getElementById('code-editor');
const runButton = document.getElementById('run-button');
const exampleSelector = document.getElementById('example-selector');

if (!canvas) {
  throw new Error('missing #canvas element');
}

// Language follows the selected example's extension; edits keep the language
// of whatever example was loaded last
let currentLanguage = 'lua';

// Load example code from file
async function loadExample(filename) {
  currentLanguage = filename.endsWith('.moon') ? 'moon' : 'lua';
  try {
    // Examples change under a static server that sends no cache headers, so
    // always revalidate rather than let the browser guess at freshness
    const response = await fetch(`examples/${filename}`, { cache: 'no-cache' });
    if (!response.ok) {
      throw new Error(`Failed to fetch example: ${response.status}`);
    }
    const code = await response.text();
    codeEditor.value = code;
  } catch (err) {
    console.error('Error loading example:', err);
    codeEditor.value = '-- Failed to load example\n';
  }
}

// Load default example on page load
loadExample('default.lua');

// Handle example selection change
exampleSelector.addEventListener('change', (e) => {
  loadExample(e.target.value);
});

// Initialize Aroma and set up UI handlers
initAroma(canvas).then((aroma) => {
  let running = false;

  const runCode = async () => {
    if (running) return;
    running = true;
    runButton.disabled = true;
    try {
      let code = codeEditor.value;
      if (currentLanguage === 'moon') {
        code = await compileMoonScript(code);
      }
      aroma.runCode(code);
    } catch (err) {
      console.error('Failed to run code:', err);
    } finally {
      running = false;
      runButton.disabled = false;
    }
  };

  runButton.addEventListener('click', runCode);

  codeEditor.addEventListener('keydown', (e) => {
    if (e.ctrlKey && e.key === 'Enter') {
      e.preventDefault();
      runCode();
    }
  });

  runButton.disabled = false;
}).catch((err) => {
  console.error('Failed to boot Aroma WASM runtime', err);
});
