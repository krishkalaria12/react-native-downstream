import { useState } from 'react';
import {
  KeyboardAvoidingView,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { DownstreamMarkdown } from 'react-native-downstream';

import { CHAT_MODEL, CHAT_REASONING_EFFORT, chatSetupHint } from './chat';
import { useChat } from './useChat';

const PLACEHOLDER = 'Ask for Markdown: a table, code, or a long answer';

function Action({
  label,
  onPress,
  disabled = false,
  primary = false,
}: {
  label: string;
  onPress: () => void;
  disabled?: boolean;
  primary?: boolean;
}) {
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ disabled }}
      disabled={disabled}
      onPress={onPress}
      style={[
        styles.action,
        primary && styles.actionPrimary,
        disabled && styles.actionDisabled,
      ]}
    >
      <Text style={[styles.actionText, primary && styles.actionTextPrimary]}>
        {label}
      </Text>
    </Pressable>
  );
}

export function ChatScreen() {
  const { messages, busy, setupError, send, stop, clear } = useChat();
  const [prompt, setPrompt] = useState('');
  const submit = () => send(prompt);

  return (
    <KeyboardAvoidingView
      style={styles.screen}
      behavior={Platform.OS === 'ios' ? 'padding' : undefined}
    >
      <View style={styles.header}>
        <Text style={styles.kicker}>Downstream / example</Text>
        <Text style={styles.title}>Streaming Markdown chat</Text>
        <Text style={styles.meta}>
          {CHAT_MODEL} · reasoning {CHAT_REASONING_EFFORT}
        </Text>
      </View>

      <ScrollView
        contentContainerStyle={styles.messages}
        keyboardShouldPersistTaps="handled"
      >
        {messages.length === 0 && (
          <Text style={styles.empty}>
            Type a prompt to stream a Markdown answer through the native
            session.
          </Text>
        )}
        {messages.map((message) => (
          <View key={message.id} style={styles.message}>
            <Text style={styles.role}>
              {message.role === 'user' ? 'You' : CHAT_MODEL}
            </Text>
            {message.stream ? (
              <DownstreamMarkdown
                stream={message.stream}
                onError={({ code, message: text }) => console.warn(code, text)}
              />
            ) : (
              <Text style={styles.promptText}>{message.text}</Text>
            )}
            {message.status === 'streaming' && (
              <Text style={styles.status}>streaming…</Text>
            )}
            {message.error && <Text style={styles.error}>{message.error}</Text>}
          </View>
        ))}
      </ScrollView>

      {setupError === 'missing-key' && (
        <Text style={styles.setup}>{chatSetupHint}</Text>
      )}

      <View style={styles.composer}>
        <TextInput
          accessibilityLabel="Prompt"
          placeholder={PLACEHOLDER}
          value={prompt}
          onChangeText={setPrompt}
          onSubmitEditing={submit}
          editable={!busy}
          multiline
          style={styles.input}
        />
        <View style={styles.actions}>
          {busy ? (
            <Action label="Stop" onPress={stop} primary />
          ) : (
            <Action
              label="Send"
              onPress={submit}
              disabled={prompt.trim().length === 0}
              primary
            />
          )}
          <Action
            label="Clear"
            onPress={clear}
            disabled={messages.length === 0}
          />
        </View>
      </View>
    </KeyboardAvoidingView>
  );
}

const colors = {
  paper: '#F2F1EB',
  ink: '#172D2A',
  muted: '#52645F',
  line: '#CBD4CB',
  teal: '#14695E',
  white: '#FFFFFF',
};

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: colors.paper },
  header: {
    paddingTop: Platform.OS === 'ios' ? 58 : 38,
    paddingHorizontal: 20,
    paddingBottom: 12,
  },
  kicker: {
    color: colors.teal,
    fontSize: 12,
    fontWeight: '700',
    letterSpacing: 1.2,
  },
  title: { color: colors.ink, fontSize: 28, fontWeight: '800', marginTop: 8 },
  meta: { color: colors.muted, fontSize: 13, marginTop: 4 },
  messages: { padding: 20, gap: 18, flexGrow: 1 },
  empty: { color: colors.muted, fontSize: 14, lineHeight: 21 },
  message: {
    backgroundColor: colors.white,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: colors.line,
    padding: 16,
  },
  role: {
    color: colors.teal,
    fontSize: 11,
    fontWeight: '700',
    letterSpacing: 0.6,
    marginBottom: 10,
  },
  promptText: { color: colors.ink, fontSize: 15, lineHeight: 22 },
  status: { color: colors.muted, fontSize: 12, marginTop: 8 },
  error: { color: '#9B2C2C', fontSize: 12, marginTop: 8 },
  setup: {
    color: '#9B2C2C',
    fontSize: 12,
    paddingHorizontal: 20,
    paddingBottom: 8,
  },
  composer: {
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: colors.line,
    paddingHorizontal: 20,
    paddingTop: 12,
    paddingBottom: Platform.OS === 'ios' ? 28 : 16,
  },
  input: {
    borderWidth: 1,
    borderColor: colors.line,
    borderRadius: 10,
    backgroundColor: colors.white,
    color: colors.ink,
    fontSize: 15,
    lineHeight: 21,
    maxHeight: 130,
    padding: 12,
  },
  actions: { flexDirection: 'row', gap: 8, marginTop: 10 },
  action: {
    borderWidth: 1,
    borderColor: colors.ink,
    borderRadius: 8,
    paddingVertical: 10,
    paddingHorizontal: 16,
  },
  actionPrimary: { backgroundColor: colors.teal, borderColor: colors.teal },
  actionDisabled: { opacity: 0.35 },
  actionText: { color: colors.ink, fontSize: 14, fontWeight: '700' },
  actionTextPrimary: { color: colors.white },
});
