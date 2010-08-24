<?php
// $Id$
/**
 * @file sphinx_rt.admin.module
 * TODO: Enter file description here.
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
  
  $form['general'] = array(
    '#type'        => 'fieldset',
    '#title'       => t('General settings'),
    '#collapsible' => TRUE,
    '#collapsed'   => FALSE,
  );
  $form['general']['sphinx_rt_node_types'] = array(
    '#type'          => 'checkboxes',
    '#title'         => t('Node types'),
    '#default_value' => variable_get('sphinx_rt_node_types', array()),
    '#options'       => $node_types_options,
    '#description'   => t('Select node types, that should been included in Sphinx RT index.'),
  );
  $form['general']['sphinx_rt_include_comments'] = array(
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
  $form['other'] = array(
    '#type'        => 'fieldset',
    '#title'       => t('Other'),
    '#collapsible' => TRUE,
    '#collapsed'   => FALSE,
  );
  $form['other']['reindex'] = array(
    '#type' => 'submit',
    '#value' => t('(Re)Index all nodes that available for searching'),
    '#submit' => array('sphinx_rt_admin_reindex'),
  );

  return system_settings_form($form);
}

/**
 * (Re)indexing all nodes through BatchAPI.
 */
function sphinx_rt_admin_reindex($form, &$form_state) {
  $allowed_node_types = variable_get('sphinx_rt_node_types', array());
    
  if (!empty($allowed_node_types)) {
    $node_types = array();
    $nodes = array();
    
    foreach ($allowed_node_types as $type => $allowed) {
      if ($allowed) {
        $node_types[] = $type;
      }
    }

    $node_types_array_str = implode("','", $node_types);
       
    $max = db_result(db_query("SELECT COUNT(*) FROM {node} WHERE type IN ('" . $node_types_array_str . "') AND status = 1"));
    $batch = array(
      'operations' => array(
        array('sphinx_rt_reindex_deleting_process', array($max, $node_types_array_str)),
        array('sphinx_rt_reindex_inserting_process', array($max, $node_types_array_str)),
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

function sphinx_rt_reindex_deleting_process($max, $node_types_array_str, &$context) {
  if (!isset($context['sandbox']['progress'])) {
    $context['sandbox']['progress'] = 0;
    $context['sandbox']['max']      = $max;
  }

  // For this example, we decide that we can safely process
  // 100 nodes at a time without a timeout.
  $limit = 100;

  // With each pass through the callback, retrieve the next group of nids.
  $nodes = array();
  $result = db_query_range("SELECT nid FROM {node} WHERE type IN ('" . $node_types_array_str . "') AND status = 1", $context['sandbox']['progress'], $limit);
  while ($row = db_fetch_array($result)) {
    // Here we actually perform our processing on the current node.
    $nodes[] = $row['nid'];

    // Update our progress information.
    $context['sandbox']['progress']++;
    $context['message'] = t('Cleaning index. Please be patient');
  }

  sphinx_rt_node_delete($nodes);

  // Inform the batch engine that we are not finished,
  // and provide an estimation of the completion level we reached.
  if ($context['sandbox']['progress'] != $context['sandbox']['max']) {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }
}

function sphinx_rt_reindex_inserting_process($max, $node_types_array_str, &$context) {
  if (!isset($context['sandbox']['progress'])) {
    $context['sandbox']['progress'] = 0;
    $context['sandbox']['max']      = $max;
  }

  // For this example, we decide that we can safely process
  // 100 nodes at a time without a timeout.
  $limit = 100;

  // With each pass through the callback, retrieve the next group of nids.
  $nodes = array();
  $result = db_query_range("SELECT nid FROM {node} WHERE type IN ('" . $node_types_array_str . "') AND status = 1", $context['sandbox']['progress'], $limit);
  while ($row = db_fetch_array($result)) {

    // Here we actually perform our processing on the current node.
    //$node = node_load($row['nid'], NULL, TRUE);
    $nodes[] = $row['nid'];

    // Store some result for post-processing in the finished callback.
    $context['results'][] = $row['nid'];

    // Update our progress information.
    $context['sandbox']['progress']++;
    $context['message'] = t('Indexing. Please be patient');
  }

  sphinx_rt_node_insert($nodes);

  // Inform the batch engine that we are not finished,
  // and provide an estimation of the completion level we reached.
  if ($context['sandbox']['progress'] != $context['sandbox']['max']) {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }
}

function sphinx_rt_reindex_finished($success, $results, $operations) {
  if ($success) {
    // Here we do something meaningful with the results.
    $message = count($results) .' nodes were added to index.';
  }
  else {
    // An error occurred.
    // $operations contains the operations that remained unprocessed.
    $error_operation = reset($operations);
    $message = t('An error occurred while processing %error_operation with arguments: @arguments', array('%error_operation' => $error_operation[0], '@arguments' => print_r($error_operation[1], TRUE)));
  }
  drupal_set_message($message);
}

/**
 * Implementation of hook_node_operations().
 */
function sphinx_rt_node_operations() {
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
  /*$nodes = array();
  while ($row = each($nids)) {
    $nodes[] = node_load($row['value']);
  }
  sphinx_rt_node_insert($nodes);*/
  sphinx_rt_node_update($nids);
}

/**
 * Implementation of a SphinxRT node operation.
 * Add or update node in Sphinx RT index.
 */
function sphinx_rt_delete_node_operation($nids) {
  /*$nodes = array();
  while ($row = each($nids)) {
    $nodes[] = node_load($row['value']);
  }
  sphinx_rt_node_delete($nodes);*/
  sphinx_rt_node_delete($nids);
}
