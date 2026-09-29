import { createOpenAI } from '@ai-sdk/openai';
import { streamText } from 'ai';

/**
 * The example only needs a fast, short Markdown answer, so it uses the cheapest
 * Luna tier with the lowest reasoning effort. Both are overridable so the build
 * can be pointed at another model without a code change.
 */
export const CHAT_MODEL = process.env.EXPO_PUBLIC_OPENAI_MODEL ?? 'gpt-6-luna';

export const CHAT_REASONING_EFFORT = 'low';

/**
 * Expo inlines `EXPO_PUBLIC_` variables at build time, so this has to be a
 * static member access. The key ships inside the bundle; the example is meant to
 * run locally, never from a store build.
 */
const API_KEY = process.env.EXPO_PUBLIC_OPENAI_API_KEY ?? '';

export const chatConfigured = API_KEY.length > 0;

export const chatSetupHint =
  'Add EXPO_PUBLIC_OPENAI_API_KEY to example/.env, then restart the bundler.';

const openai = createOpenAI({ apiKey: API_KEY });

export interface ChatTurn {
  role: 'user' | 'assistant';
  content: string;
}

/**
 * Streams one assistant reply as Markdown deltas. The caller appends each
 * delta, so the view never waits for the whole message.
 */
export async function streamChatReply({
  messages,
  signal,
  onDelta,
}: {
  messages: ChatTurn[];
  signal: AbortSignal;
  onDelta: (delta: string) => void;
}): Promise<void> {
  const result = streamText({
    model: openai(CHAT_MODEL),
    messages,
    providerOptions: {
      openai: { reasoningEffort: CHAT_REASONING_EFFORT },
    },
    abortSignal: signal,
    // A callback keeps the whole reply on promises. `for await` over
    // `textStream` would need async iteration, which Hermes only added
    // recently, and the streamdown parser already uses the same machinery.
    onChunk: ({ chunk }) => {
      if (chunk.type === 'text-delta') onDelta(chunk.text);
    },
  });
  await result.consumeStream();
}
