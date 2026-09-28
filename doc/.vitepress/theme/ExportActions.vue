<script setup lang="ts">
import { computed } from 'vue'
import { useData, withBase } from 'vitepress'

const { page } = useData()
const markdownUrl = computed(() => withBase(`/markdown/${page.value.relativePath}`))

function printPage() {
  window.print()
}
</script>

<template>
  <div class="doc-actions" aria-label="导出本文">
    <span class="doc-actions__label">本文</span>
    <a class="doc-actions__link" :href="markdownUrl" :download="page.relativePath.split('/').pop()">
      <svg aria-hidden="true" viewBox="0 0 20 20" fill="none"><path d="M4 3.5h8l4 4V16a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V4.5a1 1 0 0 1 1-1Z" stroke="currentColor" stroke-width="1.5"/><path d="M12 3.5V8h4M6.5 13.5V10l1.5 2 1.5-2v3.5M12 10h1a1.75 1.75 0 1 1 0 3.5h-1V10Z" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/></svg>
      下载 Markdown
    </a>
    <button class="doc-actions__link" type="button" @click="printPage">
      <svg aria-hidden="true" viewBox="0 0 20 20" fill="none"><path d="M5.5 6V2.75h9V6M5.5 14H3.75A1.75 1.75 0 0 1 2 12.25v-4.5C2 6.78 2.78 6 3.75 6h12.5c.97 0 1.75.78 1.75 1.75v4.5c0 .97-.78 1.75-1.75 1.75H14.5M5.5 11.5h9v5.75h-9V11.5Z" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"/><circle cx="15.25" cy="9" r=".75" fill="currentColor"/></svg>
      导出 PDF
    </button>
  </div>
</template>
