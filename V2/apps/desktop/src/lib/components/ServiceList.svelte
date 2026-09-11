<script lang="ts">
  import { api, type ServiceInfo } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, s } from "$lib/state.svelte";

  function select(svc: ServiceInfo) {
    if (!svc.is_audio) return;
    api.selectService(svc.sid, svc.scids).catch((e) => notify("warn", tError(e)));
  }
  function dragStart(svc: ServiceInfo, e: DragEvent) {
    e.dataTransfer?.setData("text/plain", `dab-service:${svc.sid}:${svc.scids}:${svc.name.trim()}`);
    if (e.dataTransfer) e.dataTransfer.effectAllowed = "copy";
  }
  const isActive = (svc: ServiceInfo) => !!s.current && s.current.sid === svc.sid && s.current.scids === svc.scids;
</script>

<section class="panel services">
  <div class="panel-head">
    <span>{t("panel.services")}</span>
    <span class="grow"></span>
    <span>{s.services.length ? t("services.count", { n: s.services.length }) : t("services.empty")}</span>
  </div>
  <div class="list">
    {#each s.services as svc (`${svc.sid}:${svc.scids}`)}
      <button
        class="row"
        class:active={isActive(svc)}
        class:data={!svc.is_audio}
        draggable={svc.is_audio}
        ondragstart={(e) => dragStart(svc, e)}
        onclick={() => select(svc)}
        ondblclick={() => select(svc)}
      >
        <span class="grow">{svc.name.trim()}{svc.scids ? ` (${svc.scids})` : ""}</span>
        <span class="meta">{svc.is_audio ? `${svc.bitrate_kbps} kbps` : t("services.data")}</span>
        <span class="meta">{svc.sid.toString(16).toUpperCase().padStart(4, "0")}</span>
      </button>
    {/each}
  </div>
</section>

<style>
  .services { display: flex; flex-direction: column; flex: 1 1 120px; min-height: 80px; }
  .list { flex: 1; margin: 3px 4px; }
  .row.data { color: #2f6a3a; }
</style>
