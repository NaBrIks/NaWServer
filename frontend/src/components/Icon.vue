<script setup lang="ts">
import helpCircle from '../design/icons/help-circle.svg?raw';
import deviceDesktop from '../design/icons/device-desktop.svg?raw';
import mcp from '../design/icons/mcp.svg?raw';
defineProps<{name:string}>();
const svgIcons:Record<string,string>={'help-circle':helpCircle,'device-desktop':deviceDesktop,mcp};
</script>
<template><i v-if="svgIcons[name]" class="svg-icon" aria-hidden="true" v-html="svgIcons[name]"></i><i v-else :class="['ti','ti-'+name]" aria-hidden="true"></i></template>
