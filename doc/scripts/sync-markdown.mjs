import { mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const target = join(root, '.vitepress', 'dist', 'markdown')
const ignored = new Set(['.vitepress', 'node_modules', 'public', 'scripts'])

await rm(target, { recursive: true, force: true })

async function copyPages(directory) {
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    if (entry.isDirectory()) {
      if (!ignored.has(entry.name)) await copyPages(join(directory, entry.name))
      continue
    }
    if (!entry.isFile() || !entry.name.endsWith('.md')) continue
    const source = join(directory, entry.name)
    const destination = join(target, relative(root, source))
    await mkdir(dirname(destination), { recursive: true })
    let content = await readFile(source, 'utf8')
    const snippets = [...content.matchAll(/^<<<\s+(\S+)\{([\w-]+)\}\s*$/gm)]
    for (const snippet of snippets) {
      const sample = await readFile(resolve(dirname(source), snippet[1]), 'utf8')
      content = content.replace(snippet[0], `\`\`\`${snippet[2]}\n${sample.trimEnd()}\n\`\`\``)
    }
    await writeFile(destination, content)
  }
}

await copyPages(root)
