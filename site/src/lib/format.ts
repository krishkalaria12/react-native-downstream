/** A number with a fixed count of decimals and thousands separators, such as 1,348.0. */
export const formatNumber = (value: number, decimals: number) =>
  value.toLocaleString('en-US', { minimumFractionDigits: decimals, maximumFractionDigits: decimals });
