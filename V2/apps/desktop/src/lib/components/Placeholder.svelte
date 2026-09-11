<script lang="ts">
  import { t } from "$lib/i18n.svelte";
  let { title }: { title: string } = $props();
</script>

<section class="panel">
  <div class="panel-head"><span>{title}</span></div>
  <div class="panel-body dim">{t("panel.placeholder")}</div>
</section>

<style>
  .dim { color: var(--text-dim); font-size: 10px; }
</style>
