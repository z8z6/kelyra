<script setup lang="ts">
import DefaultTheme from 'vitepress/theme'
import ExportActions from './ExportActions.vue'

const { Layout } = DefaultTheme
</script>

<template>
  <Layout>
    <template #doc-before>
      <ExportActions />
    </template>
  </Layout>
</template>
