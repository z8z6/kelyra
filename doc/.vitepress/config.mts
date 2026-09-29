import { defineConfig } from 'vitepress'
import { readFileSync } from 'node:fs'

const base = process.env.DOCS_BASE || '/'
const kelyraGrammar = {
  ...JSON.parse(readFileSync(new URL('../../kide/vscode/syntaxes/kelyra.tmLanguage.json', import.meta.url), 'utf8')),
  name: 'kelyra'
}

export default defineConfig({
  lang: 'zh-CN',
  title: 'Kelyra',
  description: 'Kelyra 语言文档：从第一行代码到系统接口与图形着色器。',
  base,
  lastUpdated: true,
  cleanUrls: true,
  markdown: { languages: [kelyraGrammar], codeCopyButtonTitle: '复制代码' },
  head: [
    ['meta', { name: 'theme-color', content: '#ffffff' }],
    ['link', { rel: 'icon', href: `${base}favicon.svg`, type: 'image/svg+xml' }]
  ],
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: 'Kelyra',
    nav: [
      { text: '开始学习', link: '/guide/getting-started' },
      { text: '语言参考', link: '/reference/syntax' },
      { text: '标准库与图形', link: '/library/overview' },
      { text: 'GitHub', link: 'https://github.com/z8z6/kelyra' }
    ],
    sidebar: {
      '/guide/': [
        { text: '开始学习', items: [
          { text: '认识 Kelyra', link: '/guide/overview' },
          { text: '安装与第一个程序', link: '/guide/getting-started' },
          { text: '用 Kelp 管理项目', link: '/guide/kelp' }
        ] },
        { text: '接下来', items: [
          { text: '语言参考', link: '/reference/syntax' },
          { text: '标准库', link: '/library/overview' }
        ] }
      ],
      '/reference/': [
        { text: '语言参考', items: [
          { text: '语法速览', link: '/reference/syntax' },
          { text: '类型与数据', link: '/reference/types' },
          { text: '控制流', link: '/reference/control-flow' },
          { text: '函数与重载', link: '/reference/functions' },
          { text: '类与接口', link: '/reference/classes' },
          { text: '模块、注解与平台', link: '/reference/modules' },
          { text: 'C 互操作', link: '/reference/interop' }
        ] }
      ],
      '/library/': [
        { text: '标准库与图形', items: [
          { text: '标准库概览', link: '/library/overview' },
          { text: '窗口与 UI', link: '/library/ui' },
          { text: 'Shader 与图形', link: '/library/shader' }
        ] }
      ]
    },
    outline: { level: [2, 3], label: '本页内容' },
    sidebarMenuLabel: '目录',
    returnToTopLabel: '返回顶部',
    darkModeSwitchLabel: '外观',
    lightModeSwitchTitle: '切换到浅色模式',
    darkModeSwitchTitle: '切换到深色模式',
    search: { provider: 'local', options: { translations: {
      button: { buttonText: '搜索文档', buttonAriaLabel: '搜索文档' },
      modal: {
        noResultsText: '没有找到结果，试试换个词吧。',
        resetButtonTitle: '清除搜索',
        footer: { selectText: '选择', navigateText: '切换', closeText: '关闭' }
      }
    } } },
    docFooter: { prev: '上一篇', next: '下一篇' },
    lastUpdated: { text: '最近更新' },
    editLink: { pattern: 'https://github.com/z8z6/kelyra/edit/main/doc/:path', text: '帮助改进此页' },
    footer: { message: '认真写代码，也认真写说明。', copyright: 'Kelyra contributors' },
    socialLinks: [{ icon: 'github', link: 'https://github.com/z8z6/kelyra' }]
  }
})
