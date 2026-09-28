import { existsSync } from 'node:fs'
import { readFile, readdir } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const dist = join(root, '.vitepress', 'dist')
const missing = []

async function check(directory) {
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name)
    if (entry.isDirectory()) {
      await check(path)
      continue
    }
    if (!entry.isFile() || !entry.name.endsWith('.html')) continue

    const html = await readFile(path, 'utf8')
    for (const match of html.matchAll(/(?:href|src)="(\/[^"?#]*)(?:[?#][^"]*)?"/g)) {
      const url = decodeURIComponent(match[1])
      if (url.startsWith('//')) continue
      const local = join(dist, url.slice(1))
      const target = url.endsWith('/') ? join(local, 'index.html') : local
      if (!existsSync(target) && !existsSync(`${target}.html`)) {
        missing.push(`${path}: ${url}`)
      }
    }
  }
}

await check(dist)
if (missing.length) {
  throw new Error(`Broken local links:\n${missing.join('\n')}`)
}
console.log('All local HTML links resolve inside the generated site.')
