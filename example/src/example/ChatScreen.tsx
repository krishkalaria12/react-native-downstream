import { useCallback, useRef, useState, type ComponentRef } from 'react';
import {
  KeyboardAvoidingView,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { Ionicons } from '@expo/vector-icons';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { DownstreamMarkdown } from 'react-native-downstream';

import { CHAT_MODEL, CHAT_REASONING_EFFORT, chatSetupHint } from './chat';
import type { ChatMessage } from './useChat';
import { useChat } from './useChat';
import { Badge, Button, Input, Notice } from '../ui/kit';
import {
  APP_BAR_HEIGHT,
  Hero,
  Screen,
  useBottomInset,
  useScreenScroll,
} from '../ui/screen';
import { radius, space, useTheme } from '../ui/theme';
import type { Tone } from '../ui/theme';

const PLACEHOLDER = 'Ask for a table, some code, or a long answer';

const STARTERS = [
  'A table comparing full parse with incremental tail, five rows',
  'Explain the worker thread model in three short sections',
  'A long answer with headings, lists, a table, and a code block',
] as const;

const STATUS_TONE: Record<ChatMessage['status'], Tone> = {
  streaming: 'live',
  complete: 'neutral',
  stopped: 'warn',
  error: 'danger',
};

const STATUS_LABEL: Record<ChatMessage['status'], string> = {
  streaming: 'streaming',
  complete: 'done',
  stopped: 'stopped',
  error: 'failed',
};

/** The user's own prompt, set apart from the rendered answer. */
function Prompt({ text }: { text: string }) {
  const t = useTheme();
  return (
    <View style={[styles.prompt, { backgroundColor: t.c.accent }]}>
      <Text style={[t.s.body, { color: t.c.onAccent }]}>{text}</Text>
    </View>
  );
}

function Answer({ message }: { message: ChatMessage }) {
  const t = useTheme();
  return (
    <View style={t.s.card}>
      <View style={styles.answerHead}>
        <View style={styles.answerWho}>
          <Ionicons name="sparkles-outline" size={15} color={t.c.accent} />
          <Text numberOfLines={1} style={[t.s.label, styles.answerName]}>
            {CHAT_MODEL}
          </Text>
        </View>
        <Badge
          label={STATUS_LABEL[message.status]}
          tone={STATUS_TONE[message.status]}
          pulse={message.status === 'streaming'}
        />
      </View>
      <View style={styles.answerBody}>
        {message.stream ? (
          <DownstreamMarkdown
            stream={message.stream}
            animated
            caret="block"
            onError={({ code, message: text }) => console.warn(code, text)}
          />
        ) : (
          <Text style={t.s.body}>Nothing came back for this prompt.</Text>
        )}
      </View>
      {message.error !== null && (
        <View style={[styles.answerError, { borderTopColor: t.c.lineSoft }]}>
          <Text style={[t.s.caption, { color: t.c.danger }]}>
            {message.error}
          </Text>
        </View>
      )}
    </View>
  );
}

function Starters({ onPick }: { onPick: (value: string) => void }) {
  const t = useTheme();
  return (
    <View style={styles.starters}>
      <Text style={[t.s.caption, styles.startersLabel]}>Try one of these</Text>
      {STARTERS.map((starter) => (
        <Pressable
          key={starter}
          accessibilityRole="button"
          accessibilityLabel={starter}
          accessibilityHint="Fills the composer with this prompt"
          onPress={() => onPick(starter)}
          style={({ pressed }) => [
            styles.starter,
            { backgroundColor: t.c.surface, borderColor: t.c.line },
            pressed && styles.pressed,
          ]}
        >
          <Text style={[t.s.body, styles.starterText]}>{starter}</Text>
          <Ionicons name="arrow-up-outline" size={15} color={t.c.inkFaint} />
        </Pressable>
      ))}
    </View>
  );
}

export function ChatScreen() {
  const { messages, busy, setupError, send, stop, clear } = useChat();
  const [prompt, setPrompt] = useState('');
  const scroll = useScreenScroll();
  const list = useRef<ComponentRef<typeof ScrollView>>(null);
  const insets = useSafeAreaInsets();
  const t = useTheme();
  const bottom = useBottomInset(space.lg);

  const submit = useCallback(() => {
    if (prompt.trim().length === 0) return;
    send(prompt);
    setPrompt('');
  }, [prompt, send]);

  // A streaming answer grows past the fold, so the list follows it down.
  const follow = useCallback(() => {
    list.current?.scrollToEnd({ animated: true });
  }, []);

  return (
    <Screen title="Example" scroll={scroll}>
      <KeyboardAvoidingView
        style={styles.flex}
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
        keyboardVerticalOffset={insets.top + APP_BAR_HEIGHT}
      >
        <ScrollView
          ref={list}
          onScroll={scroll.onScroll}
          scrollEventThrottle={16}
          onContentSizeChange={follow}
          contentContainerStyle={styles.thread}
          keyboardShouldPersistTaps="handled"
          keyboardDismissMode="interactive"
        >
          <Hero
            title="Example"
            summary="Ask a question and watch a Markdown answer arrive a token at a time, parsed off the main thread."
            meta={
              <Text style={[t.s.meta, styles.modelMeta]}>
                {CHAT_MODEL} · reasoning {CHAT_REASONING_EFFORT}
              </Text>
            }
          />

          {messages.length === 0 ? (
            <Starters onPick={setPrompt} />
          ) : (
            <View
              style={[
                styles.thread,
                styles.messages,
                { paddingBottom: bottom },
              ]}
            >
              {messages.map((message) =>
                message.role === 'user' ? (
                  <Prompt key={message.id} text={message.text} />
                ) : (
                  <Answer key={message.id} message={message} />
                )
              )}
            </View>
          )}
        </ScrollView>

        {setupError === 'missing-key' && (
          <View style={styles.setup}>
            <Notice tone="warn" title="No API key">
              {chatSetupHint}
            </Notice>
          </View>
        )}

        <View
          style={[
            styles.composer,
            {
              backgroundColor: t.c.canvas,
              borderTopColor: t.c.line,
              paddingBottom: insets.bottom + space.md,
            },
          ]}
        >
          <View style={styles.composerRow}>
            <Input
              accessibilityLabel="Prompt"
              placeholder={PLACEHOLDER}
              value={prompt}
              onChangeText={setPrompt}
              multiline
              editable={!busy}
              style={styles.composerInput}
            />
            {busy ? (
              <Button
                label="Stop"
                variant="danger"
                onPress={stop}
                style={styles.send}
                hint="Stops the reply and closes the document"
              />
            ) : (
              <Button
                label="Send"
                variant="primary"
                onPress={submit}
                disabled={prompt.trim().length === 0}
                style={styles.send}
                hint="Streams the reply through a native session"
              />
            )}
          </View>
          {messages.length > 0 && (
            <View style={styles.composerFoot}>
              <Text style={[t.s.caption, styles.composerNote]}>
                Deltas go straight to the native session, so a long answer never
                re-renders the list.
              </Text>
              <Button label="New chat" variant="ghost" onPress={clear} />
            </View>
          )}
        </View>
      </KeyboardAvoidingView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  flex: { flex: 1 },
  pressed: { opacity: 0.6 },
  thread: { paddingTop: space.sm },

  modelMeta: { marginTop: space.md },

  starters: {
    paddingHorizontal: space.xl,
    paddingTop: space.xxl,
    gap: space.sm,
  },
  startersLabel: { marginBottom: space.xs },
  starter: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.md,
    padding: space.lg,
    borderRadius: radius.card,
    borderWidth: 1,
  },
  starterText: { flex: 1, fontSize: 14, lineHeight: 20 },

  messages: {
    paddingHorizontal: space.xl,
    paddingTop: space.xxl,
    gap: space.md,
  },
  prompt: {
    alignSelf: 'flex-end',
    maxWidth: '88%',
    paddingHorizontal: space.lg,
    paddingVertical: space.md,
    borderRadius: radius.card,
  },

  answerHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
    paddingHorizontal: space.lg,
    paddingVertical: space.md,
  },
  answerWho: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    flexShrink: 1,
  },
  answerName: { flexShrink: 1 },
  answerBody: { paddingHorizontal: space.lg, paddingBottom: space.lg },
  answerError: {
    paddingHorizontal: space.lg,
    paddingVertical: space.md,
    borderTopWidth: StyleSheet.hairlineWidth,
  },

  setup: { paddingHorizontal: space.xl, paddingBottom: space.md },
  composer: {
    paddingHorizontal: space.xl,
    paddingTop: space.md,
    borderTopWidth: StyleSheet.hairlineWidth,
    gap: space.md,
  },
  composerRow: { flexDirection: 'row', alignItems: 'center', gap: space.sm },
  composerInput: { flex: 1, maxHeight: 120 },
  send: { minWidth: 88 },
  composerFoot: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.lg,
  },
  composerNote: { flex: 1 },
});
