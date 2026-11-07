// Shared TechnicalShell tooltip behavior. Text only; no HTML from application data.
const selector = '[data-tooltip]';
const tooltip = document.createElement('div');
// A local HTTP device is not a secure context. This identifier links ARIA nodes,
// not authentication/session state, so a non-cryptographic fallback is sufficient.
const tooltipId = typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function'
    ? crypto.randomUUID()
    : `local-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
tooltip.id = `lu-tooltip-${tooltipId}`;
tooltip.className = 'lu-tooltip';
tooltip.setAttribute('role', 'tooltip');
tooltip.setAttribute('popover', 'manual');
tooltip.hidden = true;
let active, hovered, focused, showTimer, hideTimer, frame;
let pointer;
let dismissed;
const clamp = (n, min, max) => Math.max(min, Math.min(n, max));
const overlaps = (a, b) => a.x < b.x+b.w && a.x+a.w > b.x && a.y < b.y+b.h && a.y+a.h > b.y;
const source = target => target instanceof Element ? target.closest(selector) : null;
const popupTarget = target => target instanceof Node && tooltip.contains(target);

function decorate(root) {
    const elements = root instanceof Element && root.matches(selector) ? [root] : [];
    if (root.querySelectorAll) elements.push(...root.querySelectorAll(selector));
    for (const element of elements) {
        if (!element.matches('a[href],button,input,select,textarea,summary,[tabindex]') &&
            !element.querySelector('button,input,select,textarea,a[href]')) element.setAttribute('tabindex', '0');
    }
}
function unlink() {
    if (!active) return;
    const ids = (active.getAttribute('aria-describedby') || '').split(/\s+/).filter(id => id && id !== tooltip.id);
    if (ids.length) active.setAttribute('aria-describedby', ids.join(' '));
    else active.removeAttribute('aria-describedby');
}
function hide() {
    clearTimeout(showTimer); clearTimeout(hideTimer);
    unlink(); active = null;
    if (tooltip.matches(':popover-open')) tooltip.hidePopover();
    tooltip.hidden = true;
}
function position(reduced=false) {
    if (!active?.isConnected || !active.getClientRects().length) { hide(); return; }
    const r = active.getBoundingClientRect();
    const margin = 12, gap = 32;
    const vw = document.documentElement.clientWidth, vh = window.innerHeight;
    const w = tooltip.offsetWidth, h = tooltip.offsetHeight;
    const centerX = clamp(r.left+(r.width-w)/2, margin, vw-w-margin);
    const centerY = clamp(r.top+(r.height-h)/2, margin, vh-h-margin);
    // Reserve space for a large cursor extending down and right from its hotspot.
    const cursor = pointer && hovered === active ? { x:pointer.x-24, y:pointer.y-24, w:152, h:152 } : null;
    const below = cursor ? Math.max(r.bottom+gap, cursor.y+cursor.h+margin) : r.bottom+gap;
    const right = cursor ? Math.max(r.right+gap, cursor.x+cursor.w+margin) : r.right+gap;
    const candidates = [
        {x:centerX, y:r.top-gap-h, side:'top'},
        {x:centerX, y:below, side:'bottom'},
        {x:r.left-gap-w, y:centerY, side:'left'},
        {x:right, y:centerY, side:'right'}
    ];
    const fits = p => p.x>=margin && p.y>=margin && p.x+w<=vw-margin && p.y+h<=vh-margin;
    let best = candidates.find(p => fits(p) && (!cursor || !overlaps({...p,w,h}, cursor)));
    if (!best) {
        // At a viewport corner a very long help text may need an internally scrolling box.
        const room = [{x:margin,y:margin,side:'corner'}, {x:vw-w-margin,y:margin,side:'corner'},
            {x:margin,y:vh-h-margin,side:'corner'}, {x:vw-w-margin,y:vh-h-margin,side:'corner'}];
        best = room.find(p=>!cursor || !overlaps({...p,w,h},cursor));
        if (!best && cursor && !reduced) {
            const available = Math.max(cursor.y-margin, vh-cursor.y-cursor.h-margin);
            if (available >= 80 && h > available) {
                tooltip.style.maxHeight = `${Math.floor(available)}px`;
                position(true); return;
            }
        }
        best ||= candidates[0];
    }
    tooltip.style.left = `${clamp(best.x, margin, vw-w-margin)}px`;
    tooltip.style.top = `${clamp(best.y, margin, vh-h-margin)}px`;
    tooltip.dataset.side = best.side;
}
function show(element) {
    if (!element?.isConnected || element === dismissed) return;
    const dialog = document.querySelector('dialog[open]');
    if (dialog && !dialog.contains(element)) { hide(); return; }
    const text = element.getAttribute('data-tooltip')?.trim();
    if (!text) { hide(); return; }
    const parent = element.closest('dialog[open]') || element.closest('.lu-shell') || document.body;
    if (tooltip.parentNode !== parent) { hide(); parent.append(tooltip); }
    const reanchor = active !== element;
    if (reanchor) { unlink(); active = element; }
    tooltip.textContent = text;
    tooltip.style.maxHeight = '';
    const ids = new Set((element.getAttribute('aria-describedby') || '').split(/\s+/).filter(Boolean));
    ids.add(tooltip.id); element.setAttribute('aria-describedby', [...ids].join(' '));
    tooltip.hidden = false;
    if (!tooltip.matches(':popover-open')) tooltip.showPopover();
    if (reanchor || !tooltip.matches(':hover')) position();
}
function schedule(element, delay=250) {
    clearTimeout(showTimer); clearTimeout(hideTimer);
    if (!element) { hide(); return; }
    if (active === element) return;
    showTimer = setTimeout(()=>show(element), delay);
}
function leave() {
    clearTimeout(showTimer); clearTimeout(hideTimer);
    hideTimer = setTimeout(()=>{
        if (popupTarget(document.elementFromPoint(pointer?.x ?? -1,pointer?.y ?? -1))) return;
        if (focused?.isConnected) show(focused); else hide();
    }, 300);
}
document.addEventListener('pointerover', event=>{
    if (event.pointerType === 'touch') return;
    pointer = {x:event.clientX,y:event.clientY};
    if (popupTarget(event.target)) { clearTimeout(hideTimer); return; }
    const element = source(event.target);
    if (element !== hovered) { dismissed = null; hovered = element; schedule(element); }
});
document.addEventListener('pointermove', event=>{
    if (event.pointerType === 'touch') return;
    pointer = {x:event.clientX,y:event.clientY};
    if (popupTarget(event.target)) return;
    if (active && hovered === active && !frame) frame=requestAnimationFrame(()=>{
        frame=null;
        if (!active) return;
        const r=tooltip.getBoundingClientRect();
        if (overlaps({x:r.left,y:r.top,w:r.width,h:r.height},{x:pointer.x-24,y:pointer.y-24,w:152,h:152})) position();
    });
});
document.addEventListener('pointerout', event=>{
    if (popupTarget(event.relatedTarget)) { clearTimeout(hideTimer); return; }
    const next = source(event.relatedTarget);
    if (next === hovered && !popupTarget(event.target)) return;
    hovered = next;
    if (next) schedule(next); else leave();
});
document.addEventListener('focusin', event=>{
    focused = source(event.target); dismissed = null;
    if (focused) { clearTimeout(showTimer); clearTimeout(hideTimer); show(focused); }
});
document.addEventListener('focusout', event=>{
    focused = source(event.relatedTarget);
    if (!focused && !hovered && !popupTarget(event.relatedTarget)) hide();
});
document.addEventListener('keydown', event=>{
    if (event.key === 'Escape' && active) {
        dismissed=active; hide(); event.preventDefault(); event.stopPropagation();
    }
}, true);
document.addEventListener('pointerdown', event=>{ if (!source(event.target) && !popupTarget(event.target)) hide(); },true);
document.addEventListener('scroll', event=>{ if (!popupTarget(event.target)) { dismissed=active; hide(); } },true);
document.addEventListener('close', hide,true);
window.addEventListener('resize', hide);
window.addEventListener('blur', hide);
const observer = new MutationObserver(records=>{
    for (const record of records) {
        if (record.type === 'childList') for (const node of record.addedNodes) decorate(node);
        if (record.type === 'attributes') decorate(record.target);
    }
    if (active) {
        const dialog=document.querySelector('dialog[open]');
        if (!active.isConnected || (dialog && !dialog.contains(active))) hide();
        else if (tooltip.textContent !== active.getAttribute('data-tooltip')?.trim()) show(active);
    }
});
decorate(document);
observer.observe(document.body,{subtree:true,childList:true,attributes:true,attributeFilter:['data-tooltip','open']});
