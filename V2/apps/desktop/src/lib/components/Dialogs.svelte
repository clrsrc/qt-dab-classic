<script lang="ts">
  import { closeConfirm, closeMenu, dialogs } from "$lib/dialogs.svelte";
  import { ui } from "$lib/state.svelte";
  import { t } from "$lib/i18n.svelte";

  function onKey(e: KeyboardEvent) {
    if (dialogs.confirm) {
      if (e.key === "Escape") closeConfirm(false);
      if (e.key === "Enter") closeConfirm(true);
      e.stopPropagation();
    } else if (dialogs.menu && e.key === "Escape") {
      closeMenu();
    }
  }
</script>

<svelte:window onkeydown={onKey} onmousedown={() => dialogs.menu && closeMenu()} onblur={closeMenu} />

{#if dialogs.confirm}
  <div class="overlay" onmousedown={(e) => e.target === e.currentTarget && closeConfirm(false)} role="presentation">
    <div class="dialog" role="dialog" aria-modal="true">
      <h3>{dialogs.confirm.title}</h3>
      <p>{dialogs.confirm.text}</p>
      <div class="actions">
        <button class="btn" onclick={() => closeConfirm(false)}>{dialogs.confirm.cancel}</button>
        <button class="btn on" onclick={() => closeConfirm(true)}>{dialogs.confirm.ok}</button>
      </div>
    </div>
  </div>
{/if}

{#if dialogs.menu}
  <div class="menu" style="left:{Math.min(dialogs.menu.x, window.innerWidth - 180)}px; top:{Math.min(dialogs.menu.y, window.innerHeight - 60)}px" role="menu" tabindex="-1" onmousedown={(e) => e.stopPropagation()}>
    {#each dialogs.menu.items as item (item.label)}
      <button disabled={item.disabled} onclick={() => { closeMenu(); item.action(); }}>{item.label}</button>
    {/each}
  </div>
{/if}

<div class="notices">
  {#each ui.notices as n (n.id)}
    <div class="notice {n.level}">
      {#if n.text.startsWith("core_restarted:")}
        {t("core.restarted", { reason: n.text.split(":")[1], attempt: n.text.split(":")[2] })}
      {:else}{n.text}{/if}
    </div>
  {/each}
</div>
