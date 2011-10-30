<?php
// $Id$
/**
 * @file sphinx_rt.admin.module
 * Administration functions.
 */

/**
 * Menu callback. SphinxRT administration form.
 */
function sphinx_rt_admin(&$form_state) {
  $node_types = node_get_types();
  $node_types_options = array();
  foreach ($node_types as $type) {
    $node_types_options[$type->type] = $type->name;
  }

  $vocabularies = taxonomy_get_vocabularies();
  $taxonomy_options = array();
  foreach ($vocabularies as $vocabulary) {
    $taxonomy_options[$vocabulary->vid] = $vocabulary->name;
  }

  $form['connection'] = array(
    '#type'        => 'fieldset',
    '#title'       => t('Connection settings'),
    '#collapsible' => TRUE,
    '#collapsed'   => FALSE,
  );
  $form['connection']['sphinx_rt_host'] = array (
    '#type'          => 'textfield',
    '#title'         => t('Host'),
    '#default_value' => variable_get('sphinx_rt_host', '127.0.0.1'),
    '#description'   => t('Hostname or IP-address where searchd running.'),
    '#required'      => TRUE,
  );
  $form['connection']['sphinx_rt_port'] = array (
    '#type'          => 'textfield',
    '#title'         => t('Port'),
    '#default_value' => variable_get('sphinx_rt_port', '9306'),
    '#description'   => t('Port number where searchd running.'),
    '#required'      => TRUE,
  );
  $form['connection']['sphinx_rt_index'] = array (
    '#type'          => 'textfield',
    '#title'         => t('Index'),
    '#default_value' => variable_get('sphinx_rt_index', 'rt'),
    '#description'   => t('Index name for queries.'),
    '#required'      => TRUE,
  );
  $form['indexing'] = array(
    '#type'        => 'fieldset',
    '#title'       => t('Indexing settings'),
    '#collapsible' => TRUE,
    '#collapsed'   => FALSE,
  );
  $form['indexing']['sphinx_rt_node_types'] = array(
    '#type'          => 'checkboxes',
    '#title'         => t('Node types'),
    '#default_value' => variable_get('sphinx_rt_node_types', array()),
    '#options'       => $node_types_options,
    '#description'   => t('Select node types, that should been included in Sphinx RT index.'),
  );
  $form['indexing']['sphinx_rt_include_comments'] = array(
    '#type'          => 'checkbox',
    '#title'         => t('Include comments'),
    '#default_value' => variable_get('sphinx_rt_include_comments', TRUE),
    '#description'   => t('Include comments of node to index.'),
  );
  // Taxonomy needed MVAs.
  /*$form['general']['sphinx_rt_taxonomy'] = array(
    '#type'          => 'checkboxes',
    '#title'         => t('Taxonomy'),
    '#default_value' => variable_get('sphinx_rt_taxonomy', array()),
    '#options'       => $taxonomy_options,
    '#description'   => t('Select taxonomy vocabularies, that should been included in Sphinx RT index.'),
  );*/

  return system_settings_form($form);
}

function sphinx_rt_status(&$form_state) {
  $form['indexing'] = array(
    '#type'  => 'fieldset',
    '#title' => t('Indexing status'),
  );
  $form['indexing']['reindex'] = array(
    '#type'   => 'submit',
    '#value'  => t('(Re)Index all nodes that available for searching'),
    '#submit' => array('sphinx_rt_reindex'),
    '#prefix' => '<p><strong>' . t('<em>!percent</em> of the site has been indexed.', array('!percent' => sphinx_rt_get_status_percent() . '%')) . '</strong></p>',
  );
  $form['server'] = array(
    '#type'        => 'fieldset',
    '#title'       => t('Server status'),
    '#collapsible' => TRUE,
    '#collapsed'   => FALSE,
  );
  $form['server']['variables'] = array(
    '#value' => sphinx_rt_get_status_info(),
  );
  return $form;
}

/**
 * (Re)indexing all nodes through BatchAPI.
 */
function sphinx_rt_reindex($form, &$form_state) {
  // Get node types, that allowed for indexing
  $allowed_node_types = variable_get('sphinx_rt_node_types', array());

  // If any type is allowed, start batch processing
  if (!empty($allowed_node_types)) {
    $node_types = array();
    foreach ($allowed_node_types as $type => $allowed) {
      if ($allowed) {
        $node_types[] = $type;
      }
    }

    $node_types_array_str = implode("','", $node_types);
    $max = db_result(db_query("SELECT COUNT(*) FROM {node} WHERE type IN ('" . $node_types_array_str . "') AND status = 1"));

    $batch = array(
      'operations' => array(
        array('sphinx_rt_reindex_deleting_process', array($max, $node_types_array_str)),  // Deleting all nodes from index
        array('sphinx_rt_reindex_inserting_process', array($max, $node_types_array_str)), // Indexing all allowed nodes
      ),
      'finished'         => 'sphinx_rt_reindex_finished',
      'title'            => t('(Re)Indexing'),
      'init_message'     => t('Cleaning index. Please be patient'),
      'progress_message' => '',
      'error_message'    => t('Indexing has encountered an error.'),
    );
    batch_set($batch);
  }
}

/**
 * BatchAPI - index cleaning
 */
function sphinx_rt_reindex_deleting_process($max, $node_types_array_str, &$context) {
  if (!isset($context['sandbox']['progress'])) {
    $context['sandbox']['progress'] = 0;
    $context['sandbox']['max']      = $max;
  }

  // We can safely process 100 nodes at a time without a timeout.
  $limit = 100;

  // With each pass through the callback, retrieve the next group of nids.
  $nodes = array();
  $result = db_query_range("SELECT nid FROM {node} WHERE type IN ('" . $node_types_array_str . "') AND status = 1", $context['sandbox']['progress'], $limit);
  while ($row = db_fetch_array($result)) {
    // Collect nids
    $nodes[] = $row['nid'];

    // Update our progress information.
    $context['sandbox']['progress']++;
    $context['message'] = t('Cleaning index. Please be patient');
  }

  // Deleting all collected on this step nodes from index
  sphinx_rt_node_delete($nodes);

  // Inform the batch engine that we are not finished,
  // and provide an estimation of the completion level we reached.
  if ($context['sandbox']['progress'] != $context['sandbox']['max']) {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }
}

/**
 * BatchAPI - indexing
 */
function sphinx_rt_reindex_inserting_process($max, $node_types_array_str, &$context) {
  if (!isset($context['sandbox']['progress'])) {
    $context['sandbox']['progress'] = 0;
    $context['sandbox']['max']      = $max;
  }

  // We can safely process 100 nodes at a time without a timeout.
  $limit = 100;

  // With each pass through the callback, retrieve the next group of nids.
  $nodes = array();
  $result = db_query_range("SELECT nid FROM {node} WHERE type IN ('" . $node_types_array_str . "') AND status = 1", $context['sandbox']['progress'], $limit);
  while ($row = db_fetch_array($result)) {

    // Collect nids
    $nodes[] = $row['nid'];

    // Store some result for post-processing in the finished callback.
    $context['results'][] = $row['nid'];

    // Update our progress information.
    $context['sandbox']['progress']++;
    $context['message'] = t('Indexing. Please be patient');
  }

  // Indexing all collected on this step nodes
  sphinx_rt_node_insert($nodes);

  // Inform the batch engine that we are not finished,
  // and provide an estimation of the completion level we reached.
  if ($context['sandbox']['progress'] != $context['sandbox']['max']) {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }
}

/**
 * BatchAPI - finish
 */
function sphinx_rt_reindex_finished($success, $results, $operations) {
  if ($success) {
    $message = count($results) .' nodes were added to index.';
  }
  else {
    // An error occurred.
    $error_operation = reset($operations);
    $message = t('An error occurred while processing %error_operation with arguments: @arguments', array('%error_operation' => $error_operation[0], '@arguments' => print_r($error_operation[1], TRUE)));
  }
  drupal_set_message($message);
}

/**
 * Implementation of hook_node_operations().
 */
function sphinx_rt_node_operations() {
  if (!user_access('administer sphinx rt indexes')) {
    return;
  }

  return array(
    'sphinx_rt_add_update_node' => array(
      'label' => t('Add or update node in Sphinx RT index'),
      'callback' => 'sphinx_rt_add_update_node_operation',
    ),
    'sphinx_rt_delete_node' => array(
      'label' => t('Delete node from Sphinx RT index'),
      'callback' => 'sphinx_rt_delete_node_operation',
    ),
  );
}

/**
 * Implementation of a SphinxRT node operation.
 * Add or update node in Sphinx RT index.
 */
function sphinx_rt_add_update_node_operation($nids) {
  if (sphinx_rt_node_delete($nids)) {
    if (sphinx_rt_node_insert($nids)) {
      drupal_set_message(format_plural(count($nids), '1 node was indexed', '@count nodes were indexed.'));
    }
    else {
      drupal_set_message(t('An error occured. Please check connection to sphinxsearch daemon.'), 'error');
    }
  }
  else {
    drupal_set_message(t('An error occured. Please check connection to sphinxsearch daemon.'), 'error');
  }
}

/**
 * Implementation of a SphinxRT node operation.
 * Add or update node in Sphinx RT index.
 */
function sphinx_rt_delete_node_operation($nids) {
  if (sphinx_rt_node_delete($nids)) {
    drupal_set_message(format_plural(count($nids), '1 node was deleted from index', '@count nodes were deleted from index.'));
  }
  else {
    drupal_set_message(t('An error occured. Please check connection to sphinxsearch daemon.'), 'error');
  }
}
