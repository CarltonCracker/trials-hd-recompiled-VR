// Import path regressions without browser/game dependencies.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const source = await readFile(new URL('../../../web/import.js', import.meta.url), 'utf8');
const {safeRelative, folderFiles} = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
for (const path of ['/absolute', '../escape', 'a/../b', 'a//b', 'C:evil', 'a\\b', 'a\0b', 'a/./b', '']) {
  assert.throws(() => safeRelative(path), /Unsafe import/);
}
const paths = ['default.xex', 'data/data.pak', 'data/data_menu.pak', 'package_license.toml'];
const files = paths.map(name => ({name, webkitRelativePath: `assets/${name}`, size: 1}));
assert.deepEqual(folderFiles(files).map(entry => entry.path), paths);
assert.deepEqual(folderFiles(paths.map(name => ({name, size: 1}))).map(entry => entry.path), paths);
assert.throws(() => folderFiles(files.slice(1)), /missing default.xex/);
assert.throws(() => folderFiles([...files, files[0]]), /Duplicate/);
assert.throws(() => folderFiles([{name: '../escape', size: 1}, ...files]), /Unsafe/);
assert.throws(() => folderFiles(files.map(file => ({...file, size: 0}))), /missing/);
console.log('IMPORT JS PASSED: traversal, folder root normalization, required files and duplicates');
