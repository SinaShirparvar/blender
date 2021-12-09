/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2011, Blender Foundation.
 */

#include "COM_ColorCorrectionNode.h"
#include "COM_ColorCorrectionOperation.h"

namespace blender::compositor {

ColorCorrectionNode::ColorCorrectionNode(bNode *editor_node) : Node(editor_node)
{
  /* pass */
}

void ColorCorrectionNode::convert_to_operations(NodeConverter &converter,
                                                const CompositorContext & /*context*/) const
{
  bNode *editor_node = get_bnode();

  ColorCorrectionOperation *operation = new ColorCorrectionOperation();
  operation->set_data((NodeColorCorrection *)editor_node->storage);
  operation->set_red_channel_enabled((editor_node->custom1 & 1) != 0);
  operation->set_green_channel_enabled((editor_node->custom1 & 2) != 0);
  operation->set_blue_channel_enabled((editor_node->custom1 & 4) != 0);
  converter.add_operation(operation);

  converter.map_input_socket(get_input_socket(0), operation->get_input_socket(0));
  converter.map_input_socket(get_input_socket(1), operation->get_input_socket(1));
  converter.map_output_socket(get_output_socket(0), operation->get_output_socket(0));
}

}  // namespace blender::compositor