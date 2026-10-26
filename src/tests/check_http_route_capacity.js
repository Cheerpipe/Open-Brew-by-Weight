#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

// OpenBrewByWeightHttpLifecycle.inc promises this many spare handler slots.
const REQUIRED_SPARE_ROUTES = 10;

function checkHttpRouteCapacity(source) {
  const match = source.match(/config\.max_uri_handlers\s*=\s*(\d+);/);
  if (!match) throw new Error('HTTP server max_uri_handlers not found');
  const limit = Number(match[1]);
  const routes = (source.match(/registerHandler\(server_/g) || []).length;
  if (!routes) throw new Error('No HTTP route registrations found');
  if (limit - routes < REQUIRED_SPARE_ROUTES) {
    throw new Error(
      `${routes} routes registered with max_uri_handlers=${limit}; raise ` +
      `the limit so registration keeps ${REQUIRED_SPARE_ROUTES} spare slots`);
  }
  return {routes, limit, headroom: limit - routes};
}

if (require.main === module) {
  const root = path.resolve(__dirname, '..', '..');
  const networkDirectory = path.join(root, 'src', 'network');
  const files = [path.join(root, 'src', 'OpenBrewByWeightNetwork.cpp')]
    .concat(fs.readdirSync(networkDirectory)
      .filter((name) => name.endsWith('.inc'))
      .map((name) => path.join(networkDirectory, name)));
  const result = checkHttpRouteCapacity(
    files.map((file) => fs.readFileSync(file, 'utf8')).join('\n'));
  console.log(
    `HTTP route capacity: ${result.routes}/${result.limit} ` +
    `(${result.headroom} spare)`);
}

module.exports = {checkHttpRouteCapacity, REQUIRED_SPARE_ROUTES};
