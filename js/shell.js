import { initAroma } from './aroma.js';
import { compileMoonScript } from './moonscript.js';

// Get DOM elements
const canvas = document.getElementById('canvas');
const codeEditor = document.getElementById('code-editor');
const runButton = document.getElementById('run-button');
const exampleSelector = document.getElementById('example-selector');
const openButton = document.getElementById('open-button');
const loveInput = document.getElementById('love-input');
const projectStatus = document.getElementById('project-status');

if (!canvas) {
  throw new Error('missing #canvas element');
}

// Language follows the selected example's extension; edits keep the language
// of whatever example was loaded last
let currentLanguage = 'lua';

// Load example code from file
async function loadExample(filename) {
  if (filename.endsWith('.love')) {
    codeEditor.value = '-- examples/project, packed into a .love and run as a whole\n';
    return;
  }
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

  runButton.addEventListener('click', () => {
    projectStatus.textContent = '';
    runCode();
  });

  const runLove = async (file) => {
    projectStatus.className = '';
    projectStatus.textContent = `loading ${file.name}`;
    try {
      await aroma.runLove(await file.arrayBuffer());
      projectStatus.textContent = `running ${file.name}`;
      canvas.focus();
    } catch (err) {
      console.error('Failed to run', file.name, err);
      projectStatus.className = 'error';
      projectStatus.textContent = `${file.name}: ${err.message}`;
    }
  };

  exampleSelector.addEventListener('change', async () => {
    const name = exampleSelector.value;
    if (!name.endsWith('.love')) return;
    const response = await fetch(`examples/${name}`, { cache: 'no-cache' });
    runLove(new File([await response.blob()], name));
  });

  openButton.addEventListener('click', () => loveInput.click());
  loveInput.addEventListener('change', () => {
    if (loveInput.files[0]) runLove(loveInput.files[0]);
    // so picking the same file again still fires a change
    loveInput.value = '';
  });

  // dragover has to be cancelled for the drop to be delivered at all
  window.addEventListener('dragover', (e) => {
    e.preventDefault();
    document.body.classList.add('dragging');
  });
  window.addEventListener('dragleave', (e) => {
    if (!e.relatedTarget) document.body.classList.remove('dragging');
  });
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    document.body.classList.remove('dragging');
    const file = e.dataTransfer.files[0];
    if (file) runLove(file);
  });

  openButton.disabled = false;

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
