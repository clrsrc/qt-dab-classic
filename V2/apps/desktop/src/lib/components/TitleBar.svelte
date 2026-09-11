<script lang="ts">
  import { win } from "$lib/window";
  import { t } from "$lib/i18n.svelte";
  import { s } from "$lib/state.svelte";
</script>

<header class="titlebar" data-tauri-drag-region>
  <span class="title" data-tauri-drag-region>{t("app.title")}</span>
  <span class="tag" class:on={s.core_alive} class:err={!s.core_alive && s.core_restarts > 0} title={s.core_version}>{t("core")}</span>
  {#if s.recording}<span class="tag err">{t("status.recording")}</span>{/if}
  <button class="winbtn" onclick={() => win.minimize()} title={t("window.minimize")}>_</button>
  <button class="winbtn close" onclick={() => win.close()} title={t("window.close")}>X</button>
</header>
