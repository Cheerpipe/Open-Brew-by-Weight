const localeAssert = require('assert').strict;
const localeReference = require('../web/locales/en.json');
const localeTestRoot = path.resolve(sketchDir, '..', 'temp');
let localeTestSequence = 0;

function makeTestLocales(configure) {
  fs.mkdirSync(localeTestRoot, {recursive: true});
  const dir = path.join(
      localeTestRoot, `webui-localization-${process.pid}-${++localeTestSequence}`);
  fs.mkdirSync(dir);
  const catalogs = {en: structuredClone(localeReference)};
  if (configure) configure(catalogs);
  for (const [name, catalog] of Object.entries(catalogs)) {
    fs.writeFileSync(path.join(dir, `${name}.json`), JSON.stringify(catalog));
  }
  return {dir, clean: () => fs.rmSync(dir, {recursive: true, force: true})};
}

function expectLocaleFailure(configure, pattern) {
  const fixture = makeTestLocales(configure);
  try {
    localeAssert.throws(() => webUiLocale.loadCatalog('en', {
      localesDir: fixture.dir,
    }), pattern);
  } finally {
    fixture.clean();
  }
}

localeAssert.equal(webUiLocale.loadCatalog('en').resolvedLanguage, 'en');
localeAssert.equal(webUiLocale.loadCatalog('EN').resolvedLanguage, 'en');
localeAssert.equal(webUiLocale.loadCatalog('En_en').resolvedLanguage, 'en');
for (const language of ['../en', '/en', 'en/../../x', '', 'en--us', 'e', 'english']) {
  localeAssert.throws(() => webUiLocale.normalizeLanguageCode(language),
      /language code is empty|invalid language code/);
}
localeAssert.throws(() => webUiLocale.loadCatalog('es-CL'), /no catalog for es-cl/);

{
  const key = Object.keys(localeReference.strings)[0];
  const fixture = makeTestLocales((catalogs) => {
    catalogs.en.strings[key] = `A & < > " ' café\n\\`;
    catalogs.ar = structuredClone(catalogs.en);
    Object.assign(catalogs.ar, {locale: 'ar', language: 'Arabic', direction: 'rtl'});
    catalogs['en-gb'] = structuredClone(catalogs.en);
    Object.assign(catalogs['en-gb'], {locale: 'en-gb', language: 'British English'});
  });
  try {
    const value = `A & < > " ' café\n\\`;
    const sources = [
      {file: 'text.html', type: 'html', content: `<p>{{webui:${key}}}</p>`},
      {file: 'attr.html', type: 'html', content: `<i title="{{webui:${key}}}"></i>`},
      {file: 'value.js', type: 'js', content: `const translated=__WEBUI_TEXT__("${key}")`},
      {file: 'value.css', type: 'css', content: `x{content:__WEBUI_CSS_TEXT__("${key}")}`},
      {file: 'meta.html', type: 'html',
        content: '<html lang="{{webui-meta:locale}}"{{webui-meta:direction-attribute}}>'},
      {file: 'reuse.html', type: 'html',
        content: `<b>{{webui:${key}}}</b><b>{{webui:${key}}}</b>`},
    ];
    const first = webUiLocale.renderSources(sources, {
      language: 'EN', localesDir: fixture.dir, allowUnused: true,
    });
    const second = webUiLocale.renderSources(sources, {
      language: 'en', localesDir: fixture.dir, allowUnused: true,
    });
    localeAssert.deepEqual(first.sources, second.sources);
    localeAssert.deepEqual(first.usedKeys, [key]);
    localeAssert.equal(first.sources[0].content,
        '<p>A &amp; &lt; &gt; &quot; &#39; café\n\\</p>');
    localeAssert.equal(first.sources[1].content,
        '<i title="A &amp; &lt; &gt; &quot; &#39; café\n\\"></i>');
    localeAssert.equal(
        new Function(`${first.sources[2].content};return translated`)(), value);
    localeAssert.equal(first.sources[3].content,
        `x{content:${webUiLocale.cssLiteral(value)}}`);
    localeAssert.equal(first.sources[4].content, '<html lang="en">');
    localeAssert.ok(first.sources.every((source) =>
      !/__WEBUI_|\{\{webui/.test(source.content)));
    const rtl = webUiLocale.renderSources([sources[4]], {
      language: 'ar', localesDir: fixture.dir, allowUnused: true,
    });
    localeAssert.equal(rtl.sources[0].content, '<html lang="ar" dir="rtl">');
    localeAssert.equal(webUiLocale.loadCatalog('EN_gb', {
      localesDir: fixture.dir,
    }).resolvedLanguage, 'en-gb');
  } finally {
    fixture.clean();
  }
}

expectLocaleFailure((c) => { c.en.schemaVersion = 2; }, /schemaVersion must be 1/);
expectLocaleFailure((c) => { c.en.locale = 'fr'; }, /locale must match filename en/);
expectLocaleFailure((c) => { c.en.direction = 'down'; }, /direction must be ltr or rtl/);
expectLocaleFailure((c) => {
  c.fr = structuredClone(localeReference);
  Object.assign(c.fr, {locale: 'fr', language: 'French'});
  c.fr.strings['zzzz.extra_test'] = 'extra';
}, /extra key/);
expectLocaleFailure((c) => {
  c.fr = structuredClone(localeReference);
  Object.assign(c.fr, {locale: 'fr', language: 'French'});
  delete c.fr.strings[Object.keys(c.fr.strings)[0]];
}, /missing key/);
expectLocaleFailure((c) => {
  c.en.strings[Object.keys(c.en.strings)[0]] = 7;
}, /must contain a string/);
expectLocaleFailure((c) => { c.en.strings.Bad = 'bad'; }, /invalid string key/);
expectLocaleFailure((c) => {
  c.en.strings[Object.keys(c.en.strings)[0]] = ' · ';
}, /has no words to translate/);

{
  const fixture = makeTestLocales();
  try {
    fs.writeFileSync(path.join(fixture.dir, 'fr.json'), '{');
    localeAssert.throws(() => webUiLocale.loadCatalog('fr', {
      localesDir: fixture.dir,
    }), /cannot read.*fr\.json/);
  } finally {
    fixture.clean();
  }
}

{
  const fixture = makeTestLocales();
  try {
    fs.writeFileSync(path.join(fixture.dir, 'bad_name.json'),
        JSON.stringify({...localeReference, locale: 'bad_name'}));
    localeAssert.throws(() => webUiLocale.loadCatalog('en', {
      localesDir: fixture.dir,
    }), /invalid catalog filename/);
  } finally {
    fixture.clean();
  }
}

{
  const key = Object.keys(localeReference.strings)[0];
  const options = {language: 'en', allowUnused: true};
  localeAssert.throws(() => webUiLocale.renderSources([
    {file: 'bad.html', type: 'html', content: `<a href="{{webui:${key}}}">x</a>`},
  ], options), /complete allowed quoted HTML attribute value/);
  localeAssert.throws(() => webUiLocale.renderSources([
    {file: 'bad.css', type: 'css', content: `x{color:__WEBUI_CSS_TEXT__("${key}")}`},
  ], options), /only valid as a CSS content value/);
  localeAssert.throws(() => webUiLocale.renderSources([
    {file: 'bad.js', type: 'js', content: 'const x=__WEBUI_TEXT__(key)'},
  ], options), /unresolved or invalid localization marker/);
  localeAssert.throws(() => webUiLocale.renderSources([
    {file: 'unused.html', type: 'html', content: `<p>{{webui:${key}}}</p>`},
  ], {language: 'en'}), /reference catalog has unused key/);
  const fixture = makeTestLocales((c) => {
    c.en.strings['zzzz.extra_test'] = 'extra';
  });
  try {
    localeAssert.throws(() => webUiLocale.renderSources(
        webUiLocale.projectSources(), {language: 'en', localesDir: fixture.dir}),
    /reference catalog has unused key zzzz\.extra_test/);
  } finally {
    fixture.clean();
  }
}
