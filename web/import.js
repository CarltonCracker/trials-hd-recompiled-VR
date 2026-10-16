// Local first-run import. No uploads, downloads, or replacement of existing saves.
const REQUIRED = ['default.xex', 'data/data.pak', 'data/data_menu.pak'];

export function safeRelative(path) {
  if (path.length > 4096 || path.includes('\\') || path.includes(':') || path.includes('\0') ||
      path.split('/').some(part => !part || part === '.' || part === '..')) {
    throw new Error(`Unsafe import path: ${path}`);
  }
  return path;
}

export function folderFiles(files) {
  const entries = files.map(file => {
    const path = file.importPath || file.webkitRelativePath || file.name;
    return {file, path: safeRelative(path)};
  });
  // Directory pickers include the selected directory name; drag-drop traversal doesn't.
  const prefix = entries[0]?.path.split('/')[0] + '/';
  if (!entries.some(entry => entry.path === 'default.xex') &&
      entries.every(entry => entry.path.startsWith(prefix))) {
    for (const entry of entries) entry.path = safeRelative(entry.path.slice(prefix.length));
  }
  const paths = new Set();
  for (const {path} of entries) {
    if (paths.has(path)) throw new Error(`Duplicate import path: ${path}`);
    paths.add(path);
  }
  for (const name of REQUIRED) {
    if (!entries.some(entry => entry.path === name && entry.file.size > 0)) {
      throw new Error(`Select the extracted game folder (missing ${name})`);
    }
  }
  return entries;
}

async function writeFile(directory, path, file) {
  const parts = safeRelative(path).split('/');
  for (const part of parts.slice(0, -1)) {
    directory = await directory.getDirectoryHandle(part, {create: true});
  }
  const handle = await directory.getFileHandle(parts.at(-1), {create: true});
  await file.stream().pipeTo(await handle.createWritable());
}

async function removeIfPresent(root, name) {
  try { await root.removeEntry(name, {recursive: true}); }
  catch (error) { if (error.name !== 'NotFoundError') throw error; }
}

async function hasImport(root) {
  try {
    const marker = await (await root.getFileHandle('import-ready')).getFile();
    if ((await marker.text()).trim() !== '1') return false;
    const assets = await root.getDirectoryHandle('assets');
    for (const name of REQUIRED) {
      let directory = assets;
      const parts = name.split('/');
      for (const part of parts.slice(0, -1)) directory = await directory.getDirectoryHandle(part);
      if (!(await (await directory.getFileHandle(parts.at(-1))).getFile()).size) return false;
    }
    return true;
  } catch (error) {
    if (error.name === 'NotFoundError' || error.name === 'TypeMismatchError') return false;
    throw error;
  }
}

async function droppedFiles(items) {
  const files = [];
  async function visit(handle, prefix = '') {
    if (handle.kind === 'file') {
      const file = await handle.getFile();
      Object.defineProperty(file, 'importPath', {value: prefix + file.name});
      files.push(file);
    } else {
      for await (const child of handle.values()) await visit(child, prefix + handle.name + '/');
    }
  }
  // Capture promises before the drag data store becomes unavailable.
  const handles = items.map(item => item.getAsFileSystemHandle?.());
  for (const promise of handles) {
    const handle = await promise;
    if (handle) await visit(handle);
  }
  return files;
}

async function prepareFiles() {
  const panel = document.getElementById('import-panel');
  const status = window.trialsHd.filesStatus;
  const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('trials_hd', {create: true});
  if (new URLSearchParams(location.search).get('web_assets_url') || await hasImport(root)) return;
  panel.hidden = false;
  status('Import your own Trials HD package or extracted game folder. Files stay in this browser.');
  await new Promise(resolve => {
    let busy = false;
    async function importFiles(files, folder = false) {
      if (busy || !files.length) return;
      busy = true;
      try {
        // Validate folder paths before removing any incomplete first import.
        const entries = folder ? folderFiles(files) : null;
        if (!folder && files.length !== 1) throw new Error('Select one base-game package');
        status('Copying game files to browser storage...');
        // A completed import never reaches this code. Never delete home, cache or settings.
        await removeIfPresent(root, 'import-ready');
        await removeIfPresent(root, 'assets');
        await removeIfPresent(root, 'package.stfs');
        if (entries) {
          const assets = await root.getDirectoryHandle('assets', {create: true});
          for (let i = 0; i < entries.length; ++i) {
            status(`Copying ${i + 1}/${entries.length}: ${entries[i].path}`);
            await writeFile(assets, entries[i].path, entries[i].file);
          }
        } else {
          await writeFile(root, 'package.stfs', files[0]);
        }
        const persistent = await navigator.storage.persist();
        window.trialsHd.storagePersistent = persistent;
        if (!persistent) console.warn('Browser may evict game data when storage is low; back up important saves.');
        status('Checking imported game...');
        resolve();
      } catch (error) {
        status(`Import failed: ${error.message}. Choose files to retry.`, true);
        busy = false;
      }
    }
    document.getElementById('package-file').onchange = event => importFiles([...event.target.files]);
    document.getElementById('folder-files').onchange = event => importFiles([...event.target.files], true);
    panel.ondragover = event => { event.preventDefault(); };
    panel.ondrop = async event => {
      event.preventDefault();
      try {
        const items = [...event.dataTransfer.items].filter(item => item.kind === 'file');
        const files = items.some(item => item.getAsFileSystemHandle) ?
          await droppedFiles(items) : [...event.dataTransfer.files];
        await importFiles(files, files.some(file => file.importPath?.includes('/')));
      } catch (error) { status(`Import failed: ${error.message}`, true); }
    };
  });
}

export async function startWithFiles(start) {
  if (!navigator.storage?.getDirectory || !navigator.locks) {
    throw new Error('This browser needs OPFS and Web Locks. Use current desktop Chrome/Edge on localhost.');
  }
  await navigator.locks.request('trials-hd-storage', {ifAvailable: true}, async lock => {
    if (!lock) throw new Error('Trials HD is already open in another tab. Close it before starting here.');
    await prepareFiles();
    start();
    // Page lifetime owns storage; reload/close also tears down all OPFS workers.
    await new Promise(() => {});
  });
}
