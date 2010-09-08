/* champlain-memphis-0.8.vapi generated by vapigen, do not modify. */

[CCode (cprefix = "Champlain", lower_case_cprefix = "champlain_")]
namespace Champlain {
	[CCode (cheader_filename = "champlain/champlain-memphis-renderer.h")]
	public class MemphisRenderer : Champlain.Renderer {
		[CCode (has_construct_function = false)]
		public MemphisRenderer.full (uint tile_size);
		public Clutter.Color get_background_color ();
		public unowned Champlain.BoundingBox get_bounding_box ();
		public unowned Champlain.MemphisRule get_rule (string id);
		public unowned GLib.List get_rule_ids ();
		public uint get_tile_size ();
		public void load_rules (string rules_path);
		public void remove_rule (string id);
		public void set_background_color (Clutter.Color color);
		public void set_rule (Champlain.MemphisRule rule);
		public void set_tile_size (uint size);
		[NoAccessorMethod]
		public Champlain.BoundingBox bounding_box { owned get; set; }
		public uint tile_size { get; set; }
	}
	[Compact]
	[CCode (cheader_filename = "champlain/champlain.h")]
	public class MemphisRule {
		public weak Champlain.MemphisRuleAttr border;
		public weak string keys;
		public weak Champlain.MemphisRuleAttr line;
		public weak Champlain.MemphisRuleAttr polygon;
		public weak Champlain.MemphisRuleAttr text;
		public Champlain.MemphisRuleType type;
		public weak string values;
	}
	[Compact]
	[CCode (cheader_filename = "champlain/champlain.h")]
	public class MemphisRuleAttr {
		public uchar color_alpha;
		public uchar color_blue;
		public uchar color_green;
		public uchar color_red;
		public double size;
		public weak string style;
		public uchar z_max;
		public uchar z_min;
	}
	[CCode (cprefix = "CHAMPLAIN_MEMPHIS_RULE_TYPE_", has_type_id = false, cheader_filename = "champlain/champlain.h")]
	public enum MemphisRuleType {
		UNKNOWN,
		NODE,
		WAY,
		RELATION
	}
}
