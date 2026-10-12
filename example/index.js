import { registerRootComponent } from 'expo';

// A dedicated phone benchmark entry avoids mounting the example's app screens.
const App = process.env.EXPO_PUBLIC_PHONE_BENCHMARK === '1'
  ? require('./src/bench/PhoneBenchmark').default
  : require('./src/App').default;

registerRootComponent(App);
