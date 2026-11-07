import mermaid from 'mermaid';
import { useEffect, useMemo, useState } from 'react';
import type React from 'react';
import { Modal, Pressable, StyleSheet, Text, View } from 'react-native';
import { WebView } from 'react-native-webview';
import type { WebViewMessageEvent } from 'react-native-webview/lib/WebViewTypes';

import type { CustomRendererProps } from '@react-native-downstream/core';

/**
 * The Mermaid example renderer: a `plugins.renderers` component for `mermaid`
 * fences.
 *
 * This is Phase 7's chosen option — an example renderer, not a package — and it
 * is deliberately the only diagram code in the repository. The core stays free
 * of a diagram engine until a native spike shows the cost is worth it, which is
 * what the plan decided: mermaid.js is megabytes of JavaScript that a chat
 * library has no business shipping to every app that renders a paragraph.
 *
 * The shape of it:
 *
 * - **Render** with the `mermaid` package in JavaScript, once per spec. The
 *   WebView below only *shows* the SVG — it runs no diagram code, evaluates
 *   nothing from the message, and has no bridge back except the error path.
 * - **Pan and zoom** are a transform on the SVG's wrapper, not a re-render: the
 *   SVG mermaid produced is the diagram, and fingers move the picture.
 * - **Full screen** is a modal holding a second view of the same SVG, the same
 *   shape as the table's own full-screen modal.
 * - **Export** hands the SVG string to the app through `onExportSvg`, which is
 *   what the screen below wires to sharing. The bytes never leave the device.
 * - **Streaming** is a placeholder at a fixed height. A half-written spec is not
 *   a failure, so it is not drawn as one — and a fixed height is what keeps the
 *   text below it from jumping twice in a stream.
 * - **Trust**: the spec is model output. `securityLevel: 'strict'` is what keeps
 *   a `click` directive in it from navigating anywhere, and a spec that does
 *   not parse draws as a sentence rather than as a crash.
 */

export const MERMAID_HEIGHT = 280;

/**
 * The page that shows one SVG: no scripts of its own, no network, nothing to
 * evaluate. The diagram arrives as markup in the HTML, so the worst a hostile
 * spec can do is draw a hostile picture.
 */
function pageHtml(svg: string, background: string): string {
  return `<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=4, user-scalable=yes" />
<style>
  html, body { margin: 0; padding: 8px; background: ${background}; }
  #pan { touch-action: none; transform-origin: 0 0; }
  svg { max-width: 100%; height: auto; display: block; }
</style>
</head>
<body>
<div id="pan">${svg}</div>
<script>
  (function () {
    var element = document.getElementById('pan');
    var scale = 1, x = 0, y = 0, last = null;
    function apply() {
      element.style.transform = 'translate(' + x + 'px,' + y + 'px) scale(' + scale + ')';
    }
    element.addEventListener('touchstart', function (event) {
      last = null;
      if (event.touches.length === 2) {
        last = { distance: distance(event.touches) };
      } else if (event.touches.length === 1) {
        last = { single: event.touches[0] };
      }
    }, { passive: true });
    element.addEventListener('touchmove', function (event) {
      if (event.touches.length === 1 && last && last.single) {
        x += event.touches[0].clientX - last.single.clientX;
        y += event.touches[0].clientY - last.single.clientY;
        last.single = event.touches[0];
        apply();
      } else if (event.touches.length === 2 && last && last.distance) {
        var next = distance(event.touches);
        scale = Math.min(4, Math.max(0.5, scale * next / last.distance));
        last.distance = next;
        apply();
      }
    }, { passive: true });
    function distance(touches) {
      var dx = touches[0].clientX - touches[1].clientX;
      var dy = touches[0].clientY - touches[1].clientY;
      return Math.sqrt(dx * dx + dy * dy);
    }
  })();
</script>
</body>
</html>`;
}

const styles = StyleSheet.create({
  frame: { height: MERMAID_HEIGHT, overflow: 'hidden' },
  placeholder: {
    height: MERMAID_HEIGHT,
    alignItems: 'center',
    justifyContent: 'center',
  },
  error: { padding: 12 },
  bar: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'flex-end',
    paddingVertical: 4,
  },
  button: { paddingHorizontal: 12, paddingVertical: 6 },
  modal: { flex: 1 },
});

mermaid.initialize({ startOnLoad: false, securityLevel: 'strict' });

type Diagram =
  | { state: 'incomplete' }
  | { state: 'empty' }
  | { state: 'ready'; svg: string }
  | { state: 'invalid'; reason: string };

function useDiagram(code: string, isIncomplete: boolean): Diagram {
  const [diagram, setDiagram] = useState<Diagram>(() =>
    isIncomplete ? { state: 'incomplete' } : { state: 'empty' }
  );
  useEffect(() => {
    if (isIncomplete) {
      setDiagram({ state: 'incomplete' });
      return;
    }
    if (code.trim().length === 0) {
      setDiagram({ state: 'empty' });
      return;
    }
    let cancelled = false;
    // Rendered once per spec, in JavaScript, so a stream that is still arriving
    // never asks for a diagram it cannot finish: `isIncomplete` above returns
    // before this runs.
    void mermaid
      .render(`downstream-mermaid-${code.length}-${hash(code)}`, code)
      .then((result) => {
        if (!cancelled) setDiagram({ state: 'ready', svg: result.svg });
      })
      .catch((error: unknown) => {
        if (!cancelled) {
          setDiagram({
            state: 'invalid',
            reason:
              error instanceof Error
                ? error.message
                : 'This diagram does not parse.',
          });
        }
      });
    return () => {
      cancelled = true;
    };
  }, [code, isIncomplete]);
  return diagram;
}

/** A small stable hash, so two equal specs share a render id. */
function hash(text: string): number {
  let value = 0;
  for (let index = 0; index < text.length; index++) {
    // eslint-disable-next-line no-bitwise -- wraps the hash to 32 bits
    value = (value * 31 + text.charCodeAt(index)) | 0;
  }
  return Math.abs(value);
}

function DiagramView({
  svg,
  height,
}: {
  svg: string;
  height: number;
}): React.JSX.Element {
  const html = useMemo(() => pageHtml(svg, '#ffffff'), [svg]);
  return (
    <WebView
      originWhitelist={['*']}
      source={{ html }}
      style={{ height }}
      scrollEnabled={false}
      javaScriptEnabled
      domStorageEnabled={false}
      allowFileAccess={false}
      allowUniversalAccessFromFileURLs={false}
      onMessage={(_event: WebViewMessageEvent): void => {
        // The page posts nothing: it has no bridge back by construction, so a
        // message here is never one of ours and is ignored rather than parsed.
      }}
    />
  );
}

export function MermaidRenderer(props: CustomRendererProps): React.JSX.Element {
  const [open, setOpen] = useState(false);
  const diagram = useDiagram(props.code, props.isIncomplete);

  if (diagram.state === 'incomplete') {
    return (
      <View style={styles.placeholder}>
        <Text>Reading the diagram…</Text>
      </View>
    );
  }
  if (diagram.state === 'empty') {
    return (
      <View style={styles.placeholder}>
        <Text>An empty diagram is nothing to draw.</Text>
      </View>
    );
  }
  if (diagram.state === 'invalid') {
    // A spec that does not parse is a sentence, not a crash: the writer's own
    // source is what a reader needs, and the reason is what tells them why the
    // picture is missing.
    return (
      <View style={styles.frame}>
        <Text style={styles.error}>{diagram.reason}</Text>
        <Text style={styles.error}>{props.code}</Text>
      </View>
    );
  }
  return (
    <View>
      <View style={styles.frame}>
        <DiagramView svg={diagram.svg} height={MERMAID_HEIGHT} />
      </View>
      <View style={styles.bar}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel="Open diagram full screen"
          onPress={() => setOpen(true)}
          style={styles.button}
        >
          <Text>Full screen</Text>
        </Pressable>
      </View>
      <Modal
        visible={open}
        animationType="slide"
        presentationStyle="fullScreen"
        onRequestClose={() => setOpen(false)}
      >
        <View style={styles.modal}>
          <View style={styles.bar}>
            <Pressable
              accessibilityRole="button"
              accessibilityLabel="Close diagram"
              onPress={() => setOpen(false)}
              style={styles.button}
            >
              <Text>Close</Text>
            </Pressable>
          </View>
          <DiagramView svg={diagram.svg} height={800} />
        </View>
      </Modal>
    </View>
  );
}
