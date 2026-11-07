import { useCallback, useEffect, useRef, useState } from 'react';
import {
  createMarkdownStream,
  type MarkdownStreamController,
} from '@react-native-downstream/core';

import { chatConfigured, streamChatReply, type ChatTurn } from './chat';

export type ChatStatus = 'streaming' | 'complete' | 'stopped' | 'error';

export interface ChatMessage {
  id: string;
  role: 'user' | 'assistant';
  text: string;
  status: ChatStatus;
  error: string | null;
  /** Session that drives the view; null for the user's own prompt. */
  stream: MarkdownStreamController | null;
}

function describe(error: unknown): string {
  if (error instanceof Error) return error.message;
  if (typeof error === 'object' && error !== null && 'message' in error) {
    return String((error as { message: unknown }).message);
  }
  return String(error);
}

/**
 * One assistant message per stream session. Deltas go straight to the native
 * session instead of through React state, so a long answer never re-renders the
 * message list.
 */
export function useChat() {
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const [busy, setBusy] = useState(false);
  const [setupError, setSetupError] = useState<string | null>(
    chatConfigured ? null : 'missing-key'
  );
  const turns = useRef<ChatTurn[]>([]);
  const abort = useRef<AbortController | null>(null);
  const sessions = useRef(new Set<MarkdownStreamController>());
  const nextId = useRef(0);
  // Bumped by clear, so a reply that was in flight cannot write its turns back
  // into a conversation the user already discarded.
  const epoch = useRef(0);

  const patch = useCallback(
    (id: string, update: Partial<ChatMessage>) =>
      setMessages((previous) =>
        previous.map((message) =>
          message.id === id ? { ...message, ...update } : message
        )
      ),
    []
  );

  const stop = useCallback(() => {
    abort.current?.abort();
  }, []);

  const clear = useCallback(() => {
    epoch.current += 1;
    abort.current?.abort();
    abort.current = null;
    turns.current = [];
    sessions.current.forEach((session) => session.dispose());
    sessions.current.clear();
    setMessages([]);
    setBusy(false);
  }, []);

  const send = useCallback(
    (prompt: string) => {
      const question = prompt.trim();
      if (!question || abort.current) return;
      if (!chatConfigured) {
        setSetupError('missing-key');
        return;
      }
      const userId = `u${++nextId.current}`;
      const assistantId = `a${nextId.current}`;
      const session = createMarkdownStream();
      sessions.current.add(session);
      const history: ChatTurn[] = [
        ...turns.current,
        { role: 'user', content: question },
      ];
      turns.current = history;
      setMessages((previous) => [
        ...previous,
        {
          id: userId,
          role: 'user',
          text: question,
          status: 'complete',
          error: null,
          stream: null,
        },
        {
          id: assistantId,
          role: 'assistant',
          text: '',
          status: 'streaming',
          error: null,
          stream: session,
        },
      ]);
      setSetupError(null);
      setBusy(true);
      const controller = new AbortController();
      abort.current = controller;
      const sentAt = epoch.current;
      let answer = '';
      let failure: string | null = null;
      const settle = (aborted: boolean) => {
        if (epoch.current !== sentAt) return;
        // The reply will not grow again, so close the document even when it
        // stopped early; a half-open fence still gets a final layout.
        try {
          session.finish();
        } catch {
          // The session already ended; the view keeps what it rendered.
        }
        turns.current = [...history, { role: 'assistant', content: answer }];
        patch(assistantId, {
          text: answer,
          status: aborted ? 'stopped' : failure ? 'error' : 'complete',
          error: aborted ? null : failure,
        });
        setBusy(false);
        abort.current = null;
      };

      streamChatReply({
        messages: history,
        signal: controller.signal,
        onDelta: (delta) => {
          answer += delta;
          try {
            session.append(delta);
          } catch (error) {
            failure = describe(error);
            controller.abort();
          }
        },
      }).then(
        () => settle(controller.signal.aborted),
        (error: unknown) => {
          failure ??= describe(error);
          settle(controller.signal.aborted);
        }
      );
    },
    [patch]
  );

  useEffect(
    () => () => {
      abort.current?.abort();
      sessions.current.forEach((session) => session.dispose());
      sessions.current.clear();
    },
    []
  );

  return { messages, busy, setupError, send, stop, clear };
}
