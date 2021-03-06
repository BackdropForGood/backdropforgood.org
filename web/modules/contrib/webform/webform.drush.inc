<?php

/**
 * @file
 * Functions relating to Drush integration.
 */

/**
 * Implements hook_drush_command().
 */
function webform_drush_command() {
  return array(
    'webform-export' => array(
      'description' => 'Exports webform data to a file.',
      'arguments' => array(
        'nid' => 'The node ID of the webform you want to export (required)',
      ),
      'options' => array(
        'file' => 'The file path to export to (defaults to print to stdout)',
        'format' => 'The exporter format to use. Out-of-the-box this may be "delimited" or "excel".',
        'delimiter' => 'Delimiter between columns (defaults to site-wide setting). This option may need to be wrapped in quotes. i.e. --delimter="\t".',
        'components' => 'Comma-separated list of component IDs or form keys to include.' . "\n" .
                        'May also include "webform_serial", "webform_sid", "webform_time", "webform_complete_time", "webform_modified_time", "webform_draft", "webform_ip_address", "webform_uid", and "webform_username".',
        'header-keys' => 'Integer -1 for none, 0 for label (default) or 1 for form key.',
        'select-keys' => 'Integer 0 or 1 value. Set to 1 to print select list values by their form keys instead of labels.',
        'select-format' => 'Set to "separate" (default) or "compact" to determine how select list values are exported.',
        'range-type' => 'Range of submissions to export: "all", "new", "latest", "range" (by sid, default if start is supplied), "range-serial", or "range-date".',
        'range-latest' => 'Integer specifying the latest X submissions will be downloaded. Used if "range-type" is "latest" or no other range options are provided.',
        'range-start' => 'The submission ID, serial number, or start date at which to start exporting.',
        'range-end' => 'The submission ID, serial number, or end date at which to end exporting.',
        'completion-type' => 'Submissions to be included: "finished", "draft" or "all" (default).',
        'batch-size' => 'The size of batches in rows (default 10000). If encountering out of memory errors, set this number lower to export fewer submissions per batch.',
      ),
      'aliases' => array('wfx'),
    ),
    'webform-clear' => array(
      'description' => 'Clear a webform by deleting all its submissions.',
      'arguments' => array(
        'nid' => 'The node ID of the webform you want to clear (required)',
      ),
      'options' => array(
        'batch-size' => 'The size of batches in rows (default 10000). If encountering out of memory errors, set this number lower to export fewer submissions per batch.',
      ),
    ),
  );
}

/**
 * Exports a webform via drush, useful for large data dumps that would otherwise
 * time out due to memory consumption.
 *
 * @param bool|int $nid
 *   Node ID of the webform that we want to export.
 *
 * @return
 *   The value returned from drush_set_error().
 */
function drush_webform_export($nid = FALSE) {
  if (!$nid) {
    return drush_set_error('The node ID of the webform you want to export is required.');
  }
  $node = node_load($nid);
  if (!$node) {
    return drush_set_error(dt('Node !nid was not found.', array('!nid' => $nid)));
  }

  module_load_include('inc', 'webform', 'includes/webform.submissions');
  module_load_include('inc', 'webform', 'includes/webform.export');
  module_load_include('inc', 'webform', 'includes/webform.components');
  module_load_include('inc', 'webform', 'includes/webform.report');


  // Pull in options from drush to override the defaults.
  $format = drush_get_option('format', 'delimited');
  $options = webform_results_download_default_options($node, $format);
  foreach ($options as $option_name => $option_value) {
    $options[$option_name] = drush_get_option(str_replace('_', '-', $option_name), $option_value);
  }
  $options['components'] = is_array($options['components']) ? $options['components'] : explode(',', $options['components']);

  // Map form keys to cids.
  $form_keys = array();
  foreach ($node->webform['components'] as $cid => $component) {
    $form_keys[$component['form_key']] = $cid;
  }
  foreach ($options['components'] as $key => &$component) {
    if (isset($form_keys[$component])) {
      $component = $form_keys[$component];
    }
  }
  unset($component); // Drop PHP reference.

  // Get the range options.
  unset($options['range']['range_type']);
  foreach (drush_get_merged_prefixed_options('range-') as $option_name => $option_value) {
    if ($option_name == 'type' && in_array($option_value, array('all', 'new', 'latest', 'range', 'range-serial', 'range-date'))) {
      $options['range']['range_type'] = str_replace('-', '_', $option_value);
    }
    elseif (in_array($option_name, array('start', 'end', 'latest')) && is_numeric($option_value)) {
      $options['range'][$option_name] = $option_value;
    }
    elseif (in_array($option_name, array('start', 'end')) && strtotime($option_value)) {
      $options['range']['range_type'] = 'range_date';
      $options['range'][$option_name . '_date'] = $option_value;
    }
    else {
      return drush_set_error(dt('Unsupported range option or argument: !opt=!val',
                                array('!opt' => "range-$option_name", '!val' => $option_value)));
    }
  }

  // Determine the range type based on provided input, if not explicitly set.
  if (empty($options['range']['range_type'])) {
    $options['range']['range_type'] = isset($options['range']['start'])
      ? 'range'
      : (isset($options['range']['latest'])
      ? 'latest'
      : 'all');
  }

  // Set defaults for any missing range arguments.
  switch ($options['range']['range_type']) {
    case 'latest':
      if (empty($options['range']['latest'])) {
        drush_log('Argument range-latest defaulted to 100.', 'ok');
        $options['range']['latest'] = 100;
      }
      break;
    case 'range':
    case 'range_serial':
      if (empty($options['range']['start'])) {
        $options['range']['start'] = 1;
      }
      break;
    case 'range_date':
      if (empty($options['range']['start_date'])) {
        $options['range']['start_date'] = "1/1/1970";
      }
      break;
  }


  // Get the preferred completion type
  $options['range']['completion_type'] = drush_get_option('completion-type', NULL);
  if (isset($options['range']['completion_type']) && !in_array($options['range']['completion_type'], array('finished', 'draft', 'all'))) {
    return drush_set_error('Unsupported completion-type. The available options are "finished", "draft", or "all".');
  }

  // Set the export options.
  $options['range']['batch_size'] = drush_get_option('batch-size', 10000);
  $options['file_name'] = drush_get_option('file', tempnam(config_get('system.core', 'file_temporary_path')));

  $batch = webform_results_export_batch($node, $format, $options);
  batch_set($batch);
  drush_backend_batch_process();

  // If no filename was specified, print the file and delete it.
  if (drush_get_option('file', FALSE) === FALSE) {
    drush_print(file_get_contents($options['file_name']));  // The @ makes it silent.
    @unlink($options['file_name']);  // Clean up, the @ makes it silent.
  }
}

/**
 * Clears a webform via drush, useful for webforms with many submissions that
 * would otherwise fail due to time out due or memory consumption.
 *
 * @param int $nid
 *   Node ID of the webform to clear.
 */
function drush_webform_clear($nid = FALSE) {
  if (!$nid) {
    return drush_set_error('The node ID of the webform to be cleared is required.');
  }
  $node = node_load($nid);
  if (!$node) {
    return drush_set_error(dt('Node !nid was not found.', array('!nid' => $nid)));
  }
  if (!drush_confirm(dt('Clear submissions from webform "@title"?', array('@title' => $node->title)))) {
    return drush_set_error('webform-clear cancelled.');
  }

  //module_load_include('inc', 'webform', 'includes/webform.submissions');
  //module_load_include('inc', 'webform', 'includes/webform.components');
  module_load_include('inc', 'webform', 'includes/webform.report');


  // Pull in option from drush to override the default.
  $batch_size = drush_get_option('batch-size', 10000);
  $count = 0;
  while ($deleted = webform_results_clear($nid, $batch_size)) {
    $count += $deleted;
  }
  // Alas, there is no drush version of format_plural, so use the ugly "(s)".
  drush_log(dt('@count submission(s) in webform "@title" cleared.', array('@count' => $count, '@title' => $node->title)), 'ok');
}

/**
 * Implements hook_drush_sql_sync_sanitize().
 */
function webform_drush_sql_sync_sanitize($source) {
  // Fetch list of all table.
  $all_tables = drush_sql_get_class()->listTables();
  $tables_to_truncate = array('webform_submitted_data', 'webform_submissions');

  $truncate_webform_tables_query = array();
  foreach ($tables_to_truncate as $table) {
    if (in_array($table, $all_tables, TRUE)) {
      $truncate_webform_tables_query[] = 'TRUNCATE ' . $table . ';';
    }
  }

  drush_sql_register_post_sync_op('webform_submitted_data',
    dt('Delete all data submitted to webforms (depending on the site config, may contain sensitive data).'),
    implode(' ', $truncate_webform_tables_query));
}
