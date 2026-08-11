const escapedGql = gql`query \` name`;
const inExpr = `${html`<b>x</b>`} ${css`.x { color: red; }`} ${gql`query X { x }`} ${<Tag id="x"/>}`;
const inBraces = `${{ html: html`<i>x</i>`, css: css`.y { width: 1px; }`, gql: gql`query Y { y }`, jsx: <Item key="y"/> }}`;
