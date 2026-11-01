const disabled = process.env.DOWNSTREAM_DISABLE_CODE_PACKAGE === '1';
module.exports = {
  dependency: {
    platforms: {
      ios: disabled ? null : {},
      android: disabled
        ? null
        : {
            packageImportPath:
              'import com.downstream.code.DownstreamCodePackage;',
            packageInstance: 'new DownstreamCodePackage()',
          },
    },
  },
};
